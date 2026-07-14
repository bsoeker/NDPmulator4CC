#include "ndp_dev_a/ndp_dev_a.hh"

namespace gem5 {
NDPDevA::NDPDevA(const NDPDevAParams &params) : NDP(params) {}

uint64_t NDPDevA::readPI(uint64_t ridx) {
  switch (ridx) {
  case 5:
    return pi_stat_rgst;
  case 6:
    return pi_last_rslt;
  default:
    panic("NDPDevA does not have readable r[%lu] register!\n", ridx);
  }
}

void NDPDevA::writePI(uint64_t ridx, uint64_t data) {
  DPRINTF(NDPDevAPI, "NDP device PI: %lu -> r[%lu]\n", data, ridx);

  if (!pi_stat_rgst) {
    panic("Tried to start workload when previous one is not finished!\n");
  }

  switch (ridx) {
  case 0:
    pi_addr_data = data;
    break;
  case 1:
    pi_data_size = data;
    break;
  case 2:
    pi_data_skey = data;
    break;
  case 3:
    pi_cmmd_code = data;
    break;
  case 4:
    pi_stat_rgst = 0;
    scaleFactor = data;
    has_operands = false;
    process_fsm();
    break;
  default:
    panic("NDPDevA does not have writable r[%lu] register!\n", ridx);
  }
}

void NDPDevA::signal_completion() {
  schedule(new EventFunctionWrapper([this] { pi_stat_rgst = 1; },
                                    name() + ".writeBackResultsEvent", true),
           clockEdge(Cycles(1 * scaleFactor)));
}

// =======================================================================
// INITIALIZER: Fires the first memory request based on the command code
// =======================================================================
void NDPDevA::process_fsm() {
  current_depth = 0;
  current_addr = pi_addr_data;
  pi_last_rslt = 0;

  switch (pi_cmmd_code) {
  case 0: // Compare-Hit (Bulk)
  case 1: // Compare-Count (Bulk)
  case 2: // Compare-Max (Bulk)
    operands = new uint64_t[pi_data_size];
    accessMemory(Addr(pi_addr_data), pi_data_size * sizeof(uint64_t), false,
                 (uint8_t *)operands);
    break;

  case 3: // Strided Access
  case 4: // Pointer Chasing
  case 5: // Read-Modify-Write (RMW)
    // Kick off the first 64-byte cache line fetch
    accessMemory(Addr(current_addr), sizeof(Node), false,
                 (uint8_t *)&current_node_buffer);
    break;

  default:
    panic("Invalid command was issued to NDPDevA!\n");
  }
}

// =======================================================================
// CONTINUATION ENGINE: Handles incoming bus data and state transitions
// =======================================================================
void NDPDevA::recvData(Addr addr, uint8_t *data, size_t size) {
  DPRINTF(NDPDevAMem, "NDPDevA received %s callback for %u bytes at %p\n",
          data ? "READ" : "WRITE", size, addr);

  switch (pi_cmmd_code) {
  // ---------------------------------------------------------------
  // CMD 0, 1, 2: Bulk Linear Traversal
  // ---------------------------------------------------------------
  case 0:
  case 1:
  case 2: {
    uint64_t cycles = 0;
    if (pi_cmmd_code == 0)
      cycles = compare_n_hit(operands, pi_data_size, pi_data_skey);
    else if (pi_cmmd_code == 1)
      cycles = compare_n_count(operands, pi_data_size, pi_data_skey);
    else
      cycles = compare_n_max(operands, pi_data_size);

    delete[] operands;
    signal_completion();
    break;
  }

  // ---------------------------------------------------------------
  // CMD 3: Strided Access (Non-Contiguous Reads)
  // pi_data_skey acts as the Stride Length (in bytes)
  // ---------------------------------------------------------------
  case 3: {
    current_depth++;
    if (current_depth >= pi_data_size) {
      signal_completion();
    } else {
      current_addr += pi_data_skey; // Jump by stride
      accessMemory(Addr(current_addr), sizeof(Node), false,
                   (uint8_t *)&current_node_buffer);
    }
    break;
  }

  // ---------------------------------------------------------------
  // CMD 4: Pointer Chasing (Dependent Reads)
  // Data payload guides the next memory fetch
  // ---------------------------------------------------------------
  case 4: {
    current_depth++;
    Node *received = (Node *)data;
    pi_last_rslt = received->payload; // Keep track of last visited payload

    if (current_depth >= pi_data_size || received->next_addr == 0) {
      signal_completion();
    } else {
      current_addr = received->next_addr; // True dependent lookup
      accessMemory(Addr(current_addr), sizeof(Node), false,
                   (uint8_t *)&current_node_buffer);
    }
    break;
  }

  // ---------------------------------------------------------------
  // CMD 5: Read-Modify-Write (Invalidation Traffic Generator)
  // ---------------------------------------------------------------
  case 5: {
    if (data != NULL) {
      // Phase A: READ returned. Modify the payload, issue a WRITE.
      Node *received = (Node *)data;
      received->payload += pi_data_skey; // Arbitrary work
      accessMemory(addr, sizeof(Node), true,
                   (uint8_t *)received); // Note: write=true
    } else {
      // Phase B: WRITE ACK returned (data == NULL from ndp.cc). Step forward.
      current_depth++;
      if (current_depth >= pi_data_size) {
        signal_completion();
      } else {
        current_addr += sizeof(Node); // Move to next contiguous cache line
        accessMemory(Addr(current_addr), sizeof(Node), false,
                     (uint8_t *)&current_node_buffer);
      }
    }
    break;
  }

  default:
    panic("Invalid command in recvData!\n");
  }
}

// --- Legacy Implementations ---
uint64_t NDPDevA::compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey) {
  for (uint64_t i = 0; i < size; ++i) {
    if (data[i] == skey) {
      pi_last_rslt = 1;
      return i;
    }
  }
  return size;
}

uint64_t NDPDevA::compare_n_count(uint64_t *data, uint64_t size,
                                  uint64_t skey) {
  uint64_t n = 0;
  for (uint64_t i = 0; i < size; ++i) {
    if (data[i] == skey)
      n++;
  }
  pi_last_rslt = n;
  return size;
}

uint64_t NDPDevA::compare_n_max(uint64_t *data, uint64_t size) {
  uint64_t max = data[0];
  for (uint64_t i = 1; i < size; ++i) {
    if (data[i] > max)
      max = data[i];
  }
  pi_last_rslt = max;
  return size;
}
} // namespace gem5

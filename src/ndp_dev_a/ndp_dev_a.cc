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

void NDPDevA::signal_completion(uint64_t compute_cycles) {
  // Total delay = The base pipeline flush/setup time (scaleFactor) +
  // mathematical compute time
  uint64_t total_cycles = scaleFactor + compute_cycles;

  schedule(new EventFunctionWrapper([this] { pi_stat_rgst = 1; },
                                    name() + ".writeBackResultsEvent", true),
           clockEdge(Cycles(total_cycles)));
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
    operands = new uint64_t[pi_data_size];
    accessMemory(Addr(pi_addr_data), pi_data_size * sizeof(uint64_t), false,
                 (uint8_t *)operands);
    break;

  case 3: // Strided Access
  case 4: // Pointer Chasing
  case 5: // Read-Modify-Write (RMW)
          // Kick off the first 64-byte cache line fetch
    accessMemory(Addr(current_addr), 64, false, current_node_buffer);
    break;

  case 6: { // Seed pointer-chase list functionally, via DMA (no CPU cache
            // touch). Used to give ISOLATED/CLEAN mode a real chain to
            // walk without ever routing the write through the CPU's
            // ports.
    uint64_t n = pi_data_size;
    seedBuffer = new uint8_t[n * sizeof(Node)];
    Node *nodes = (Node *)seedBuffer;
    for (uint64_t i = 0; i < n; i++) {
      nodes[i].payload = i * 100;
      nodes[i].next_addr =
          (i < n - 1) ? pi_addr_data + (i + 1) * sizeof(Node) : 0;
    }
    accessMemory(Addr(pi_addr_data), n * sizeof(Node), true, seedBuffer);
    break;
  }

  case 8: { // Tree Traversal: fan-out read starting at the root.
            // pi_data_size holds the tree depth; treeTarget is the total
            // node count of a complete binary tree at that depth.
            // Termination is natural (a complete tree issues exactly
            // treeTarget reads total).
    treeVisited = 0;
    treeTarget = (1ULL << (pi_data_size + 1)) - 1;
    uint8_t *rootBuf = new uint8_t[64];
    accessMemory(Addr(pi_addr_data), 64, false, rootBuf);
    break;
  }

  case 9: { // Seed a complete binary tree functionally, via DMA (no CPU
            // cache touch). pi_data_size holds the depth; same node-
            // count formula as cmd 8. Array layout: node i's children
            // live at indices 2i+1, 2i+2.
    uint64_t depth = pi_data_size;
    uint64_t total = (1ULL << (depth + 1)) - 1;
    seedBuffer = new uint8_t[total * sizeof(TreeNode)];
    TreeNode *nodes = (TreeNode *)seedBuffer;
    for (uint64_t i = 0; i < total; i++) {
      nodes[i].payload = i * 100;
      uint64_t left = 2 * i + 1, right = 2 * i + 2;
      nodes[i].left_addr =
          (left < total) ? pi_addr_data + left * sizeof(TreeNode) : 0;
      nodes[i].right_addr =
          (right < total) ? pi_addr_data + right * sizeof(TreeNode) : 0;
    }
    accessMemory(Addr(pi_addr_data), total * sizeof(TreeNode), true,
                 seedBuffer);
    break;
  }

  default:
    panic("Invalid command was issued to NDPDevA!\n");
  }
}

// =======================================================================
// CONTINUATION ENGINE: Handles incoming bus data and state transitions
// =======================================================================
void NDPDevA::recvData(Addr addr, uint8_t *data, size_t size) {
  DPRINTF(NDPDevAMem, "CALLBACK: Addr %p, size %lu\n", addr, size);

  switch (pi_cmmd_code) {
  // ---------------------------------------------------------------
  // CMD 0: Bulk Linear Traversal
  // ---------------------------------------------------------------
  case 0: {
    compare_n_hit(operands, pi_data_size, pi_data_skey);
    delete[] operands;

    // The Analytical Delay Calculation
    uint64_t elements_per_cycle = 1; // Assuming a scalar 64-bit ALU
    uint64_t pipeline_depth = 15; // Cycles to fill/drain the compute pipeline
    uint64_t compute_cycles =
        (pi_data_size / elements_per_cycle) + pipeline_depth;

    signal_completion(compute_cycles);
    break;
  }

  // ---------------------------------------------------------------
  // CMD 3: Strided Access (Non-Contiguous Reads)
  // ---------------------------------------------------------------
  case 3: {
    current_depth++;
    if (current_depth >= pi_data_size) {
      signal_completion(
          0); // Compute delay is hidden/interleaved with memory fetches
    } else {
      current_addr += pi_data_skey;
      accessMemory(Addr(current_addr), 64, false, current_node_buffer);
    }
    break;
  }

  // ---------------------------------------------------------------
  // CMD 4: Pointer Chasing (Dependent Reads)
  // ---------------------------------------------------------------
  case 4: {
    current_depth++;
    Node *received = (Node *)data;
    pi_last_rslt = received->payload;

    if (current_depth >= pi_data_size || received->next_addr == 0) {
      signal_completion(0);
    } else {
      current_addr = received->next_addr;
      accessMemory(Addr(current_addr), 64, false, current_node_buffer);
    }
    break;
  }

  // ---------------------------------------------------------------
  // CMD 5: Read-Modify-Write (Invalidation Traffic Generator)
  // ---------------------------------------------------------------
  case 5: {
    if (data != NULL) {
      Node *received = (Node *)data;
      received->payload += pi_data_skey;
      accessMemory(addr, sizeof(Node), true, (uint8_t *)received);
    } else {
      current_depth++;
      if (current_depth >= pi_data_size) {
        signal_completion(0);
      } else {
        current_addr += sizeof(Node);
        accessMemory(Addr(current_addr), 64, false, current_node_buffer);
      }
    }
    break;
  }

  // ---------------------------------------------------------------
  // CMD 6: List-Seeding DMA Write Completion
  // ---------------------------------------------------------------
  case 6: {
    delete[] seedBuffer;
    seedBuffer = nullptr;
    signal_completion(0);
    break;
  }

  // ---------------------------------------------------------------
  // CMD 8: Tree Traversal -- each arriving node spawns reads for its
  // (up to) two children, then frees its own scratch buffer. Natural
  // termination: a complete tree issues exactly treeTarget reads total.
  // ---------------------------------------------------------------
  case 8: {
    TreeNode *node = (TreeNode *)data;
    treeVisited++;

    if (node->left_addr != 0) {
      uint8_t *leftBuf = new uint8_t[64];
      accessMemory(Addr(node->left_addr), 64, false, leftBuf);
    }
    if (node->right_addr != 0) {
      uint8_t *rightBuf = new uint8_t[64];
      accessMemory(Addr(node->right_addr), 64, false, rightBuf);
    }

    delete[] data;
    pi_last_rslt = treeVisited;

    if (treeVisited == treeTarget) {
      signal_completion(0);
    }
    break;
  }

  // ---------------------------------------------------------------
  // CMD 9: Tree-Seeding DMA Write Completion
  // ---------------------------------------------------------------
  case 9: {
    delete[] seedBuffer;
    seedBuffer = nullptr;
    signal_completion(0);
    break;
  }

  default:
    panic("Invalid command in recvData!\n");
  }
}

// --- Legacy Implementation ---
uint64_t NDPDevA::compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey) {
  for (uint64_t i = 0; i < size; ++i) {
    if (data[i] == skey) {
      pi_last_rslt = i;
      return i;
    }
  }
  pi_last_rslt = size;
  return size;
}
} // namespace gem5

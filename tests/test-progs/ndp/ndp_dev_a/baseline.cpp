#include "baseline.h"

uint64_t compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey) {
  for (uint64_t i = 0; i < size; ++i)
    if (data[i] == skey)
      return 1;
  return 0;
}

uint64_t compare_n_count(uint64_t *data, uint64_t size, uint64_t skey) {
  uint64_t n = 0;
  for (uint64_t i = 0; i < size; ++i)
    if (data[i] == skey)
      n++;
  return n;
}

uint64_t compare_n_max(uint64_t *data, uint64_t size) {
  uint64_t max = data[0];
  for (uint64_t i = 1; i < size; ++i)
    if (data[i] > max)
      max = data[i];
  return max;
}

// =====================================================================
// NEW WORKLOADS
// =====================================================================

uint64_t strided_access(uint64_t *data, uint64_t size, uint64_t stride_bytes) {
  volatile uint64_t sink; // Prevent compiler optimization
  uint64_t stride_words = stride_bytes / sizeof(uint64_t);
  for (uint64_t i = 0; i < size; i++) {
    sink = data[i * stride_words];
  }
  return 0;
}

uint64_t pointer_chase(Node *nodes, uint64_t max_depth,
                       uint64_t physical_base_addr) {
  Node *curr = &nodes[0];
  uint64_t payload = 0;
  for (uint64_t i = 0; i < max_depth; i++) {
    payload = curr->payload;
    if (curr->next_addr == 0)
      break;

    // Software needs to convert the physical address back to a virtual pointer
    uint64_t offset = curr->next_addr - physical_base_addr;
    curr = (Node *)((uint8_t *)nodes + offset);
  }
  return payload; // Returns the payload of the final node reached
}

uint64_t read_modify_write(Node *nodes, uint64_t num_nodes, uint64_t addend) {
  for (uint64_t i = 0; i < num_nodes; i++) {
    nodes[i].payload += addend;
  }
  return 0;
}

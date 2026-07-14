#ifndef __BASELINE_H__
#define __BASELINE_H__

#include <cstdint>

// 64-byte aligned structure matching the hardware cache line
struct Node {
  uint64_t next_addr; // Physical address of next node
  uint64_t payload;   // Data payload
  uint8_t padding[48];
};

uint64_t compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey);
uint64_t compare_n_count(uint64_t *data, uint64_t size, uint64_t skey);
uint64_t compare_n_max(uint64_t *data, uint64_t size);

// New workloads
uint64_t strided_access(uint64_t *data, uint64_t size, uint64_t stride_bytes);
uint64_t pointer_chase(Node *nodes, uint64_t max_depth,
                       uint64_t physical_base_addr);
uint64_t read_modify_write(Node *nodes, uint64_t num_nodes, uint64_t addend);

#endif

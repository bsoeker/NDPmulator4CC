#ifndef __NDPDevA_HH__
#define __NDPDevA_HH__

#include "debug/NDPDevA.hh"
#include "debug/NDPDevAMem.hh"
#include "debug/NDPDevAPI.hh"
#include "ndp/ndp.hh"
#include "params/NDPDevA.hh"

namespace gem5 {
class NDPDevA : public NDP {
private:
  uint64_t scaleFactor;

  // PI Registers
  uint64_t pi_addr_data = 0;
  uint64_t pi_data_size = 0;
  uint64_t pi_data_skey = 0;
  uint64_t pi_cmmd_code = 0;
  uint64_t pi_stat_rgst = 1;
  uint64_t pi_last_rslt = 0;

  // Bulk execution tracking (cmd 0)
  bool has_operands = false;
  uint64_t *operands;

  // --- State Tracking for Demand-Driven Workloads (cmd 3/4/5) ---
  struct Node {
    uint64_t payload;    // Data payload
    uint64_t next_addr;  // Physical address of next node
    uint8_t padding[48]; // Pad out to exactly 64 bytes (1 cache line)
  };

  uint8_t current_node_buffer[64];
  uint64_t current_depth = 0;
  uint64_t current_addr = 0;

  // Functional list-seeding buffer (cmd 6/9, CLEAN/ISOLATED-mode init;
  // freed on completion in recvData). Reused across both since they
  // never run concurrently within one test invocation.
  uint8_t *seedBuffer = nullptr;

  // --- Tree Traversal (cmd 8/9) ---
  // Array-layout complete binary tree: node i's children live at 2i+1,
  // 2i+2.
  struct TreeNode {
    uint64_t payload;
    uint64_t left_addr;
    uint64_t right_addr;
    uint8_t padding[40]; // 8+8+8+40 = 64 bytes
  };

  uint64_t treeVisited = 0;
  uint64_t treeTarget = 0;

  // Legacy baseline algorithm (cmd 0)
  uint64_t compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey);

  // Core state machine functions
  void process_fsm();
  void signal_completion(uint64_t compute_cycles = 0);

public:
  NDPDevA(const NDPDevAParams &params);

  uint64_t readPI(uint64_t ridx) override;
  void writePI(uint64_t ridx, uint64_t data) override;
  void recvData(Addr addr, uint8_t *data, size_t size) override;
};
} // namespace gem5

#endif //__NDPDevA_HH__

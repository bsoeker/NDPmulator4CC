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

  // Bulk execution tracking
  bool has_operands = false;
  uint64_t *operands;

  // --- NEW: State Tracking for Demand-Driven Workloads ---
  struct Node {
    uint64_t next_addr;  // Physical address of next node
    uint64_t payload;    // Data payload
    uint8_t padding[48]; // Pad out to exactly 64 bytes (1 cache line)
  };

  Node current_node_buffer; // 64-byte buffer for single-line fetches
  uint64_t current_depth = 0;
  uint64_t current_addr = 0;

  // Legacy baseline algorithms
  uint64_t compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey);
  uint64_t compare_n_count(uint64_t *data, uint64_t size, uint64_t skey);
  uint64_t compare_n_max(uint64_t *data, uint64_t size);

  // Core state machine functions
  void process_fsm();
  void signal_completion();

public:
  NDPDevA(const NDPDevAParams &params);

  uint64_t readPI(uint64_t ridx) override;
  void writePI(uint64_t ridx, uint64_t data) override;
  void recvData(Addr addr, uint8_t *data, size_t size) override;
};
} // namespace gem5

#endif //__NDPDevA_HH__

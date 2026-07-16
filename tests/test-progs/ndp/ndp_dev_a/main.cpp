#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <time.h>

// --- Memory Map & Hardware Macros ---
#define NDP_CTRL 0x40000000
#define NDP_DATA 0x40001000
#define NDP_NODES 0x40009000
#define DATA_SIZE 0x1000
#define MAX_KEY (DATA_SIZE / 4)
#define START_CODE 50

// --- Data Structures ---
struct Node {
  uint64_t payload;
  uint64_t next_addr;
  uint8_t padding[48]; // Force the struct to be exactly 64 bytes
};

// --- gem5 SE-Mode Safe Timing ---
// Uses the simulated process clock, avoiding assembly assertion crashes
inline uint64_t read_sim_ticks() {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// --- Software Baselines ---
uint64_t sw_compare_n_hit(uint64_t *data, uint64_t size, uint64_t skey) {
  for (uint64_t i = 0; i < size; ++i) {
    if (data[i] == skey)
      return i;
  }
  return size;
}

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);

  // Toggled by hand between runs: false == SHARED, true == ISOLATED
  bool run_isolated = true;

  // Hardware Interface Pointers
  uint64_t *ndp_ctrl = (uint64_t *)NDP_CTRL;
  uint64_t *ndp_data = (uint64_t *)NDP_DATA;
  volatile Node *ndp_nodes = (volatile Node *)NDP_NODES;

  uint64_t &pi_addr_data = ndp_ctrl[0];
  uint64_t &pi_data_size = ndp_ctrl[1];
  uint64_t &pi_data_skey = ndp_ctrl[2];
  uint64_t &pi_cmmd_code = ndp_ctrl[3];
  uint64_t &pi_strt_rgst = ndp_ctrl[4];
  uint64_t &pi_stat_rgst = ndp_ctrl[5];
  uint64_t &pi_last_rslt = ndp_ctrl[6];

  std::cout << "\n========== NDP ACCELERATOR MICROBENCHMARK =========="
            << std::endl;
  std::cout << "Mode: "
            << (run_isolated ? "ISOLATED (Cold/Untouched)"
                             : "SHARED (CPU Cached/Coherent)")
            << std::endl;

  uint64_t num_nodes = (DATA_SIZE * sizeof(uint64_t)) / sizeof(Node);

  // =======================================================================
  // PHASE 1: CPU Memory Initialization (Only if Shared)
  // =======================================================================
  if (!run_isolated) {
    // Init Bulk Array
    for (int i = 0; i < DATA_SIZE; ++i) {
      ndp_data[i] = rand() % MAX_KEY;
    }
    ndp_data[DATA_SIZE / 2] = MAX_KEY + 1; // Guarantee a hit

    // Init Pointer Chasing Linked List (Overlays on same memory)
    for (uint64_t i = 0; i < num_nodes; i++) {
      ndp_nodes[i].payload = i * 100;
      ndp_nodes[i].next_addr =
          (i < num_nodes - 1) ? NDP_NODES + (i + 1) * sizeof(Node) : 0;
    }
  }

  // =======================================================================
  // PHASE 2: Test Bulk Processing (Compute-Bound, Pipelined)
  // Command 0: Compare-N-Hit
  // =======================================================================
  std::cout << "\n[TEST 1] Bulk Processing (Cmd 0: Compare-N-Hit)" << std::endl;

  uint64_t start_sw_bulk = 0, end_sw_bulk = 0, res_sw_bulk = 0;
  if (!run_isolated) {
    start_sw_bulk = read_sim_ticks();
    res_sw_bulk = sw_compare_n_hit(ndp_data, DATA_SIZE, MAX_KEY + 1);
    end_sw_bulk = read_sim_ticks();
  }

  uint64_t start_hw_bulk = read_sim_ticks();
  pi_addr_data = NDP_DATA;
  pi_data_size = DATA_SIZE;
  pi_data_skey = MAX_KEY + 1;
  pi_cmmd_code = 0;
  pi_strt_rgst = START_CODE;

  while (!pi_stat_rgst)
    ; // Spin wait

  uint64_t end_hw_bulk = read_sim_ticks();
  uint64_t res_hw_bulk = pi_last_rslt;

  if (run_isolated) {
    printf("  -> HW Ticks: %lu\n", end_hw_bulk - start_hw_bulk);
  } else {
    printf("  -> SW Ticks: %lu | HW Ticks: %lu | Match: %s\n",
           end_sw_bulk - start_sw_bulk, end_hw_bulk - start_hw_bulk,
           (res_sw_bulk == res_hw_bulk) ? "PASS" : "FAIL");
  }

  // =======================================================================
  // PHASE 2.5: Test Strided Access (Memory-Bound, Independent Reads)
  // Command 3: Non-Contiguous Reads
  // =======================================================================
  std::cout << "\n[TEST 2] Strided Access (Cmd 3: Non-Contiguous Reads)"
            << std::endl;

  // Stride/count chosen to keep the total footprint (200 * 128B = 25600B)
  // comfortably inside NDP_DATA (which spans 0x40001000..0x40009000) and
  // clear of NDP_NODES. No SHARED/ISOLATED-specific setup needed here:
  // pi_data_skey is reused as the byte stride, cmd 3's termination in
  // NDPDevA::recvData is a pure depth counter (current_depth >=
  // pi_data_size), and it never inspects the fetched bytes -- so it
  // correctly inherits whatever coherency state Phase 1 already
  // established for this range in each mode, with no extra seeding.
  const uint64_t stride_bytes = 128;
  const uint64_t stride_count = 200;

  uint64_t start_hw_stride = read_sim_ticks();
  pi_addr_data = NDP_DATA;
  pi_data_size = stride_count;
  pi_data_skey = stride_bytes; // repurposed as stride step for cmd 3
  pi_cmmd_code = 3;
  pi_strt_rgst = START_CODE;

  while (!pi_stat_rgst)
    ; // Spin wait

  uint64_t end_hw_stride = read_sim_ticks();

  printf("  -> HW Ticks: %lu\n", end_hw_stride - start_hw_stride);

  // =======================================================================
  // PHASE 3: Test Pointer Chasing (Latency-Bound, Serialized)
  // Command 4: Linked List Traversal
  // =======================================================================
  std::cout << "\n[TEST 3] Pointer Chasing (Cmd 4: Dependent Reads)"
            << std::endl;

  if (run_isolated) {
    // Seed the list via the NDP device's own DMA path (cmd 6) instead of
    // the CPU. This keeps the region genuinely untouched by the CPU cache
    // hierarchy while still giving the device a real chain to walk.
    // Untimed on purpose -- symmetric with Phase 1's CPU-side init being
    // untimed in SHARED mode.
    pi_addr_data = NDP_NODES;
    pi_data_size = num_nodes;
    pi_cmmd_code = 6;
    pi_strt_rgst = START_CODE;
    while (!pi_stat_rgst)
      ; // Spin wait
  }

  uint64_t start_hw_chase = read_sim_ticks();
  pi_addr_data = NDP_NODES;
  pi_data_size = num_nodes;
  pi_data_skey = 0;
  pi_cmmd_code = 4;
  pi_strt_rgst = START_CODE;

  while (!pi_stat_rgst)
    ; // Spin wait

  uint64_t end_hw_chase = read_sim_ticks();
  uint64_t res_hw_chase = pi_last_rslt;

  printf("  -> HW Ticks: %lu\n", end_hw_chase - start_hw_chase);
  // Verification print: should read (num_nodes - 1) * 100 = %lu if the
  // full chain was actually walked. If this reads 0 (or anything other
  // than the expected value), the traversal terminated early -- check
  // that the chain was seeded correctly for the current mode.
  printf("  -> Last Payload: %lu (expected: %lu)\n", res_hw_chase,
         (num_nodes - 1) * 100);

  std::cout << "====================================================\n"
            << std::endl;

  return 0;
}

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

  bool run_isolated = false;
  // if (argc > 1 && std::string(argv[1]) == "--isolated") {
  //   run_isolated = true;
  // }

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
  // PHASE 3: Test Pointer Chasing (Latency-Bound, Serialized)
  // Command 4: Linked List Traversal
  // =======================================================================
  std::cout << "\n[TEST 2] Pointer Chasing (Cmd 4: Dependent Reads)"
            << std::endl;

  uint64_t start_hw_chase = read_sim_ticks();
  pi_addr_data = NDP_NODES;
  pi_data_size = num_nodes;
  pi_data_skey = 0;
  pi_cmmd_code = 4;
  pi_strt_rgst = START_CODE;

  while (!pi_stat_rgst)
    ; // Spin wait

  uint64_t end_hw_chase = read_sim_ticks();

  printf("  -> HW Ticks: %lu\n", end_hw_chase - start_hw_chase);
  std::cout << "====================================================\n"
            << std::endl;

  return 0;
}

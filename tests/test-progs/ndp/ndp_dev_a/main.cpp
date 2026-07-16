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

// =========================================================================
// HAND-TOGGLED CONFIG -- edit these three, recompile, run, save stats.txt,
// repeat. Each combination below gets its own clean, unpolluted stats.txt
// with exactly one test's memory activity in it.
// =========================================================================
enum TestSelect { TEST_BULK = 1, TEST_STRIDED = 2, TEST_CHASE = 3 };

static const bool run_isolated = true; // false = SHARED, true = ISOLATED
static const TestSelect test_select = TEST_CHASE; // which single test to run
static const bool run_sw_baseline = false; // only for correctness spot-checks;
                                           // leave false for stats.txt runs
                                           // -- it adds ~120k ticks of
                                           // unrelated L1/L2 traffic that
                                           // has nothing to do with NDP
                                           // coherence and will pollute
                                           // any snoop/writeback counters
                                           // you pull from this run.
// =========================================================================

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);

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
  std::cout << "Test: "
            << (test_select == TEST_BULK      ? "Bulk (Cmd 0)"
                : test_select == TEST_STRIDED ? "Strided (Cmd 3)"
                                              : "Pointer Chase (Cmd 4)")
            << std::endl;

  uint64_t num_nodes = (DATA_SIZE * sizeof(uint64_t)) / sizeof(Node);

  // =======================================================================
  // PHASE 1: CPU Memory Initialization (Only if Shared)
  // Only initializes what the selected test actually needs, so this run's
  // stats.txt doesn't carry cache traffic from data structures that test
  // never touches.
  // =======================================================================
  if (!run_isolated) {
    if (test_select == TEST_BULK || test_select == TEST_STRIDED) {
      for (int i = 0; i < DATA_SIZE; ++i) {
        ndp_data[i] = rand() % MAX_KEY;
      }
      ndp_data[DATA_SIZE / 2] = MAX_KEY + 1; // Guarantee a hit (bulk only)
    }
    if (test_select == TEST_CHASE) {
      for (uint64_t i = 0; i < num_nodes; i++) {
        ndp_nodes[i].payload = i * 100;
        ndp_nodes[i].next_addr =
            (i < num_nodes - 1) ? NDP_NODES + (i + 1) * sizeof(Node) : 0;
      }
    }
  }

  // =======================================================================
  // TEST: Bulk Processing (Cmd 0)
  // =======================================================================
  if (test_select == TEST_BULK) {
    std::cout << "\n[TEST] Bulk Processing (Cmd 0: Compare-N-Hit)" << std::endl;

    uint64_t start_sw = 0, end_sw = 0, res_sw = 0;
    if (run_sw_baseline && !run_isolated) {
      start_sw = read_sim_ticks();
      res_sw = sw_compare_n_hit(ndp_data, DATA_SIZE, MAX_KEY + 1);
      end_sw = read_sim_ticks();
    }

    uint64_t start_hw = read_sim_ticks();
    pi_addr_data = NDP_DATA;
    pi_data_size = DATA_SIZE;
    pi_data_skey = MAX_KEY + 1;
    pi_cmmd_code = 0;
    pi_strt_rgst = START_CODE;

    while (!pi_stat_rgst)
      ; // Spin wait

    uint64_t end_hw = read_sim_ticks();
    uint64_t res_hw = pi_last_rslt;

    if (run_sw_baseline && !run_isolated) {
      printf("  -> SW Ticks: %lu | HW Ticks: %lu | Match: %s\n",
             end_sw - start_sw, end_hw - start_hw,
             (res_sw == res_hw) ? "PASS" : "FAIL");
    } else {
      printf("  -> HW Ticks: %lu\n", end_hw - start_hw);
    }
  }

  // =======================================================================
  // TEST: Strided Access (Cmd 3)
  // =======================================================================
  if (test_select == TEST_STRIDED) {
    std::cout << "\n[TEST] Strided Access (Cmd 3: Non-Contiguous Reads)"
              << std::endl;

    const uint64_t stride_bytes = 128;
    const uint64_t stride_count = 200;

    uint64_t start_hw = read_sim_ticks();
    pi_addr_data = NDP_DATA;
    pi_data_size = stride_count;
    pi_data_skey = stride_bytes; // repurposed as stride step for cmd 3
    pi_cmmd_code = 3;
    pi_strt_rgst = START_CODE;

    while (!pi_stat_rgst)
      ; // Spin wait

    uint64_t end_hw = read_sim_ticks();
    printf("  -> HW Ticks: %lu\n", end_hw - start_hw);
  }

  // =======================================================================
  // TEST: Pointer Chasing (Cmd 4)
  // =======================================================================
  if (test_select == TEST_CHASE) {
    std::cout << "\n[TEST] Pointer Chasing (Cmd 4: Dependent Reads)"
              << std::endl;

    if (run_isolated) {
      // Seed via NDP's own DMA path (cmd 6) -- CPU never touches this
      // range. Untimed setup, symmetric with Phase 1 above.
      pi_addr_data = NDP_NODES;
      pi_data_size = num_nodes;
      pi_cmmd_code = 6;
      pi_strt_rgst = START_CODE;
      while (!pi_stat_rgst)
        ; // Spin wait
    }

    uint64_t start_hw = read_sim_ticks();
    pi_addr_data = NDP_NODES;
    pi_data_size = num_nodes;
    pi_data_skey = 0;
    pi_cmmd_code = 4;
    pi_strt_rgst = START_CODE;

    while (!pi_stat_rgst)
      ; // Spin wait

    uint64_t end_hw = read_sim_ticks();
    uint64_t res_hw = pi_last_rslt;

    printf("  -> HW Ticks: %lu\n", end_hw - start_hw);
    printf("  -> Last Payload: %lu (expected: %lu)\n", res_hw,
           (num_nodes - 1) * 100);
  }

  std::cout << "====================================================\n"
            << std::endl;

  return 0;
}

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
#define DATA_SIZE 0x400
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
// HAND-TOGGLED CONFIG -- edit these, recompile, run, save stats.txt, repeat.
// 3 tests x 3 share modes = 9 runs for the full dirty/clean/isolated sweep.
// =========================================================================
enum TestSelect { TEST_BULK = 1, TEST_STRIDED = 2, TEST_CHASE = 3 };
enum ShareMode {
  MODE_DIRTY = 0,   // CPU writes the data -> line ends up Modified
  MODE_CLEAN = 1,   // CPU only reads the data -> line ends up Shared
  MODE_ISOLATED = 2 // CPU never touches the data at all
};

static const ShareMode share_mode = MODE_CLEAN;
static const TestSelect test_select = TEST_BULK;
static const bool run_sw_baseline = false; // CLEAN meaningful in MODE_DIRTY +
                                           //  TEST_BULK; ignored otherwise.
                                           //  Leave false for stats.txt runs.
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

  const char *mode_name =
      share_mode == MODE_DIRTY   ? "DIRTY (CPU-Written, Modified state)"
      : share_mode == MODE_CLEAN ? "CLEAN (CPU-Read-Only, Shared state)"
                                 : "ISOLATED (Cold/Untouched)";
  const char *test_name = test_select == TEST_BULK ? "Bulk (Cmd 0)"
                          : test_select == TEST_STRIDED
                              ? "Strided (Cmd 3)"
                              : "Pointer Chase (Cmd 4)";

  std::cout << "\n========== NDP ACCELERATOR MICROBENCHMARK =========="
            << std::endl;
  std::cout << "Mode: " << mode_name << std::endl;
  std::cout << "Test: " << test_name << std::endl;

  uint64_t num_nodes = (DATA_SIZE * sizeof(uint64_t)) / sizeof(Node);

  // =======================================================================
  // PHASE 1: Untimed setup -- establishes the cache-residency state that
  // the timed test below will observe. Only initializes what the selected
  // test actually needs.
  // =======================================================================
  volatile uint64_t touch_sink = 0; // prevents the compiler from eliding
                                    // the CLEAN-mode read-only sweeps below

  if (share_mode == MODE_DIRTY) {
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
  } else if (share_mode == MODE_CLEAN) {
    if (test_select == TEST_CHASE) {
      // Seed the real chain via NDP's own DMA path (cmd 6) -- CPU must NOT
      // be the one to write this data, or the line ends up Modified
      // instead of Shared once the CPU reads it below.
      pi_addr_data = NDP_NODES;
      pi_data_size = num_nodes;
      pi_cmmd_code = 6;
      pi_strt_rgst = START_CODE;
      while (!pi_stat_rgst)
        ; // Spin wait
    }

    // CPU touches the data with reads only -- pulls lines into cache as
    // Shared, never Modified. This is the one variable that differs from
    // MODE_DIRTY: same end state (data cache-resident), different MOESI
    // state, so any tick difference between DIRTY and CLEAN on the same
    // test isolates the dirty-line-handling cost specifically.
    if (test_select == TEST_BULK || test_select == TEST_STRIDED) {
      for (int i = 0; i < DATA_SIZE; ++i) {
        touch_sink = ndp_data[i];
      }
    }
    if (test_select == TEST_CHASE) {
      for (uint64_t i = 0; i < num_nodes; i++) {
        touch_sink = ndp_nodes[i].payload;
      }
    }
  }
  // MODE_ISOLATED: no Phase 1 setup at all. TEST_CHASE seeds its chain
  // immediately before its own timed section below, same as before.

  // =======================================================================
  // TEST: Bulk Processing (Cmd 0)
  // =======================================================================
  if (test_select == TEST_BULK) {
    std::cout << "\n[TEST] Bulk Processing (Cmd 0: Compare-N-Hit)" << std::endl;

    uint64_t start_sw = 0, end_sw = 0, res_sw = 0;
    if (run_sw_baseline && share_mode == MODE_DIRTY) {
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

    if (run_sw_baseline && share_mode == MODE_DIRTY) {
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

    if (share_mode == MODE_ISOLATED) {
      // Seed via NDP's own DMA path (cmd 6) -- CPU never touches this
      // range. Untimed setup, right before the timed section, since
      // MODE_ISOLATED skips Phase 1 above entirely.
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

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
#define NDP_TREE                                                               \
  0x40011000 // Right after NDP_NODES' used range
             // (0x40009000 + 512*64 = 0x40011000) --
             // no overlap.
#define DATA_SIZE 0x1000
#define MAX_KEY (DATA_SIZE / 4)
#define START_CODE 50
#define TREE_DEPTH 8 // 2^9 - 1 = 511 nodes, close to chase's 512 scale

// --- DRAM Cooldown (CLEAN mode only) ---
#define COOLDOWN_REGION 0x60000000
#define COOLDOWN_LINES 32768 // 32768 * 64B = 2MB swept

// --- Data Structures ---
struct Node {
  uint64_t payload;
  uint64_t next_addr;
  uint8_t padding[48]; // Force the struct to be exactly 64 bytes
};

struct TreeNode {
  uint64_t payload;
  uint64_t left_addr;
  uint64_t right_addr;
  uint8_t padding[40]; // 8+8+8+40 = 64 bytes
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
// HAND-TOGGLED CONFIG -- edit these, recompile, run, save console output &
// stats.txt, repeat.
// =========================================================================
enum TestSelect { TEST_BULK, TEST_STRIDED, TEST_CHASE, TEST_RMW, TEST_TREE };
enum ShareMode {
  MODE_DIRTY,   // CPU writes the data -> line ends up Modified
  MODE_CLEAN,   // CPU only reads the data -> line ends up Shared
  MODE_ISOLATED // CPU never touches the data at all
};

static const ShareMode share_mode = MODE_DIRTY;
static const TestSelect test_select = TEST_TREE;
static const bool run_sw_baseline = false; // only meaningful in MODE_DIRTY +
                                           // TEST_BULK; ignored otherwise.
                                           // Leave false for stats.txt runs.
// =========================================================================

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);

  uint64_t *ndp_ctrl = (uint64_t *)NDP_CTRL;
  uint64_t *ndp_data = (uint64_t *)NDP_DATA;
  volatile Node *ndp_nodes = (volatile Node *)NDP_NODES;
  volatile TreeNode *ndp_tree = (volatile TreeNode *)NDP_TREE;

  uint64_t &pi_addr_data = ndp_ctrl[0];
  uint64_t &pi_data_size = ndp_ctrl[1];
  uint64_t &pi_data_skey = ndp_ctrl[2];
  uint64_t &pi_cmmd_code = ndp_ctrl[3];
  uint64_t &pi_strt_rgst = ndp_ctrl[4];
  uint64_t &pi_stat_rgst = ndp_ctrl[5];
  uint64_t &pi_last_rslt = ndp_ctrl[6];

  const char *mode_name =
      share_mode == MODE_DIRTY   ? "DIRTY (CPU-Written, Modified state)"
      : share_mode == MODE_CLEAN ? "CLEAN (CPU-Read-Only, Shared state, "
                                   "DRAM-cooled)"
                                 : "ISOLATED (Cold/Untouched)";
  const char *test_name =
      test_select == TEST_BULK      ? "Bulk (Cmd 0)"
      : test_select == TEST_STRIDED ? "Strided (Cmd 3, serialized)"
      : test_select == TEST_CHASE   ? "Pointer Chase (Cmd 4)"
      : test_select == TEST_RMW     ? "Read-Modify-Write (Cmd 5)"
                                    : "Tree Traversal (Cmd 8, branching "
                                      "fan-out reads)";

  std::cout << "\n========== NDP ACCELERATOR MICROBENCHMARK =========="
            << std::endl;
  std::cout << "Mode: " << mode_name << std::endl;
  std::cout << "Test: " << test_name << std::endl;

  uint64_t num_nodes = (DATA_SIZE * sizeof(uint64_t)) / sizeof(Node);
  uint64_t tree_total = (1ULL << (TREE_DEPTH + 1)) - 1;

  // =======================================================================
  // PHASE 1: Untimed setup.
  // =======================================================================
  volatile uint64_t touch_sink = 0;

  if (share_mode == MODE_DIRTY) {
    if (test_select == TEST_BULK || test_select == TEST_STRIDED) {
      for (int i = 0; i < DATA_SIZE; ++i) {
        ndp_data[i] = rand() % MAX_KEY;
      }
      ndp_data[DATA_SIZE / 2] = MAX_KEY + 1; // Guarantee a hit (bulk only)
    }
    if (test_select == TEST_CHASE || test_select == TEST_RMW) {
      for (uint64_t i = 0; i < num_nodes; i++) {
        ndp_nodes[i].payload = i * 100;
        ndp_nodes[i].next_addr =
            (i < num_nodes - 1) ? NDP_NODES + (i + 1) * sizeof(Node) : 0;
      }
    }
    if (test_select == TEST_TREE) {
      for (uint64_t i = 0; i < tree_total; i++) {
        ndp_tree[i].payload = i * 100;
        uint64_t left = 2 * i + 1, right = 2 * i + 2;
        ndp_tree[i].left_addr =
            (left < tree_total) ? NDP_TREE + left * sizeof(TreeNode) : 0;
        ndp_tree[i].right_addr =
            (right < tree_total) ? NDP_TREE + right * sizeof(TreeNode) : 0;
      }
    }
  } else if (share_mode == MODE_CLEAN) {
    if (test_select == TEST_CHASE || test_select == TEST_RMW) {
      pi_addr_data = NDP_NODES;
      pi_data_size = num_nodes;
      pi_cmmd_code = 6;
      pi_strt_rgst = START_CODE;
      while (!pi_stat_rgst)
        ; // Spin wait
    }
    if (test_select == TEST_TREE) {
      pi_addr_data = NDP_TREE;
      pi_data_size = TREE_DEPTH;
      pi_cmmd_code = 9;
      pi_strt_rgst = START_CODE;
      while (!pi_stat_rgst)
        ; // Spin wait
    }

    // CPU touches the data with reads only -- pulls lines into cache as
    // Shared, never Modified.
    if (test_select == TEST_BULK || test_select == TEST_STRIDED) {
      for (int i = 0; i < DATA_SIZE; ++i) {
        touch_sink = ndp_data[i];
      }
    }
    if (test_select == TEST_CHASE || test_select == TEST_RMW) {
      for (uint64_t i = 0; i < num_nodes; i++) {
        touch_sink = ndp_nodes[i].payload;
      }
    }
    if (test_select == TEST_TREE) {
      for (uint64_t i = 0; i < tree_total; i++) {
        touch_sink = ndp_tree[i].payload;
      }
    }

    // DRAM cooldown --  without this, a tight
    // burst of requests into memory the touch sweep JUST activated gets
    // an unearned row-buffer speed boost, biasing CLEAN faster than
    // ISOLATED.
    volatile uint64_t *cooldown_ptr = (volatile uint64_t *)COOLDOWN_REGION;
    volatile uint64_t cooldown_sink = 0;
    for (uint64_t i = 0; i < COOLDOWN_LINES; i++) {
      cooldown_sink = cooldown_ptr[i * 8];
    }
  }
  // MODE_ISOLATED: no Phase 1 setup at all.

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
  // TEST: Strided Access (Cmd 3, serialized)
  // =======================================================================
  if (test_select == TEST_STRIDED) {
    std::cout << "\n[TEST] Strided Access (Cmd 3: Non-Contiguous Reads)"
              << std::endl;

    const uint64_t stride_bytes = 128;
    const uint64_t stride_count = 200;

    uint64_t start_hw = read_sim_ticks();
    pi_addr_data = NDP_DATA;
    pi_data_size = stride_count;
    pi_data_skey = stride_bytes;
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

  // =======================================================================
  // TEST: Read-Modify-Write (Cmd 5)
  // =======================================================================
  if (test_select == TEST_RMW) {
    std::cout << "\n[TEST] Read-Modify-Write (Cmd 5: Invalidation Traffic)"
              << std::endl;

    const uint64_t addend = 1;

    uint64_t start_hw = read_sim_ticks();
    pi_addr_data = NDP_NODES;
    pi_data_size = num_nodes;
    pi_data_skey = addend;
    pi_cmmd_code = 5;
    pi_strt_rgst = START_CODE;

    while (!pi_stat_rgst)
      ; // Spin wait

    uint64_t end_hw = read_sim_ticks();
    printf("  -> HW Ticks: %lu\n", end_hw - start_hw);

    uint64_t expected =
        (share_mode == MODE_ISOLATED) ? addend : (num_nodes - 1) * 100 + addend;
    uint64_t actual = ndp_nodes[num_nodes - 1].payload;
    printf("  -> Last Node Payload: %lu (expected: %lu)\n", actual, expected);
  }
  // =======================================================================
  // TEST: Tree Traversal (Cmd 8) -- branching fan-out reads
  // =======================================================================
  if (test_select == TEST_TREE) {
    std::cout << "\n[TEST] Tree Traversal (Cmd 8: Branching Fan-Out Reads)"
              << std::endl;

    if (share_mode == MODE_ISOLATED) {
      // Data-dependent termination (left/right_addr == 0 checks at
      // leaves) needs real seeded data, same reasoning as chase.
      pi_addr_data = NDP_TREE;
      pi_data_size = TREE_DEPTH;
      pi_cmmd_code = 9;
      pi_strt_rgst = START_CODE;
      while (!pi_stat_rgst)
        ; // Spin wait
    }

    uint64_t start_hw = read_sim_ticks();
    pi_addr_data = NDP_TREE;
    pi_data_size = TREE_DEPTH;
    pi_cmmd_code = 8;
    pi_strt_rgst = START_CODE;

    while (!pi_stat_rgst)
      ; // Spin wait

    uint64_t end_hw = read_sim_ticks();
    uint64_t res_hw = pi_last_rslt; // treeVisited count at completion

    printf("  -> HW Ticks: %lu\n", end_hw - start_hw);
    printf("  -> Nodes Visited: %lu (expected: %lu)\n", res_hw, tree_total);
  }

  std::cout << "====================================================\n"
            << std::endl;

  return 0;
}

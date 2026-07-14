#include <chrono>
#include <cstdint>
#include <iostream>

#ifdef FS
#include <cassert>
#include <fcntl.h>
#include <sys/mman.h>
#endif

#if defined(FS) && defined(DRIVER)
#include <unistd.h>
#endif

#include "baseline.h"

#define NDP_CTRL 0x40000000
#define NDP_DATA 0x40001000
#define NDP_CSZE 0x1000
#define NDP_TSZE 0x40000000
#define FID_NAME "/dev/ndp_dev_a"
#define WRI_SIZE 0x28
#define REA_SIZE 0x10

#define START_CODE 50

// Size set to fit comfortably inside caches to avoid eviction panics
#define DATA_SIZE 0x1000
#define MAX_KEY (DATA_SIZE / 4)

#define GET_TICKS std::chrono::high_resolution_clock::now()
#define GET_ELAPS(A, B)                                                        \
  std::chrono::duration_cast<std::chrono::nanoseconds>(B - A).count()

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL); // Flush stdout immediately

#ifdef FS
  int fd_dev_mem = open("/dev/mem", O_RDWR | O_SYNC);
  assert(fd_dev_mem);

  uint64_t *ndp_mreg =
      (uint64_t *)mmap(NULL, (size_t)NDP_TSZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_dev_mem, NDP_CTRL);

#ifdef DRIVER
  uint64_t ndp_ctrl[7];

  int fd_ndp_dev_a = open(FID_NAME, O_RDWR | O_SYNC);
  assert(fd_ndp_dev_a);
#else
  uint64_t *ndp_ctrl = ndp_mreg;
#endif

  uint64_t *ndp_data = ndp_mreg + (NDP_CSZE / sizeof(uint64_t));
#else
  uint64_t *ndp_ctrl = (uint64_t *)NDP_CTRL;
  uint64_t *ndp_data = (uint64_t *)NDP_DATA;
#endif

  // Initialize data for legacy queries
  for (int i = 0; i < DATA_SIZE; ++i) {
    ndp_data[i] = rand() % MAX_KEY;
  }
  ndp_data[0] = MAX_KEY + 1;
  ndp_data[DATA_SIZE / 2] = MAX_KEY + 2;
  ndp_data[DATA_SIZE - 1] = MAX_KEY + 3;

  // Initialize Linked List Nodes for Pointer Chasing
  Node *ndp_nodes = (Node *)ndp_data;
  uint64_t num_nodes = (DATA_SIZE * sizeof(uint64_t)) / sizeof(Node);

  for (uint64_t i = 0; i < num_nodes; i++) {
    ndp_nodes[i].payload = i * 100;
    if (i < num_nodes - 1) {
      ndp_nodes[i].next_addr = NDP_DATA + (i + 1) * sizeof(Node);
    } else {
      ndp_nodes[i].next_addr = 0;
    }
  }

  uint64_t &pi_addr_data = ndp_ctrl[0];
  uint64_t &pi_data_size = ndp_ctrl[1];
  uint64_t &pi_data_skey = ndp_ctrl[2];
  uint64_t &pi_cmmd_code = ndp_ctrl[3];
  uint64_t &pi_strt_rgst = ndp_ctrl[4];
  uint64_t &pi_stat_rgst = ndp_ctrl[5];
  uint64_t &pi_last_rslt = ndp_ctrl[6];

  std::cout << "========== WORKLOAD STARTED ========" << std::endl;

  /* ============================== Hit_TSC_Best ==============================
   */
  //   auto start_sw = GET_TICKS;
  //   uint64_t res_sw = compare_n_hit(ndp_data, DATA_SIZE, MAX_KEY + 1);
  //   auto end_sw = GET_TICKS;
  //
  //   auto start_hw = GET_TICKS;
  //   pi_addr_data = NDP_DATA;
  // #if defined(FS)
  //   pi_data_size = 0x40;
  // #else
  //   pi_data_size = 0x1;
  // #endif
  //   pi_data_skey = MAX_KEY + 1;
  //   pi_cmmd_code = 0;
  //   pi_strt_rgst = START_CODE;
  // #if defined(FS) && defined(DRIVER)
  //   assert(write(fd_ndp_dev_a, (void *)ndp_ctrl, WRI_SIZE) == WRI_SIZE);
  //   do {
  //     assert(read(fd_ndp_dev_a, (void *)(ndp_ctrl + WRI_SIZE /
  //     sizeof(uint64_t)),
  //                 REA_SIZE) == REA_SIZE);
  //   } while (!pi_stat_rgst);
  // #else
  //   while (!pi_stat_rgst)
  //     ;
  // #endif
  //   uint64_t res_hw = pi_last_rslt;
  //   auto end_hw = GET_TICKS;
  //
  //   printf("[%s] Hit_TSC_Best:  sw: %6lu ns, hw: %6lu ns (norm: %2.3f)\n",
  //          res_sw == res_hw ? "PASS" : "FAIL", GET_ELAPS(start_sw, end_sw),
  //          GET_ELAPS(start_hw, end_hw),
  //          1.0 * GET_ELAPS(start_hw, end_hw) / GET_ELAPS(start_sw, end_sw));

  /* ============================== Strided_TSC ==============================
   */
  //   uint64_t stride_bytes = 128;
  //   uint64_t strided_elements = DATA_SIZE / (stride_bytes /
  //   sizeof(uint64_t));
  //
  //   start_sw = GET_TICKS;
  //   res_sw = strided_access(ndp_data, strided_elements, stride_bytes);
  //   end_sw = GET_TICKS;
  //
  //   start_hw = GET_TICKS;
  //   pi_addr_data = NDP_DATA;
  //   pi_data_size = strided_elements;
  //   pi_data_skey = stride_bytes;
  //   pi_cmmd_code = 3;
  //   pi_strt_rgst = START_CODE;
  // #if defined(FS) && defined(DRIVER)
  //   assert(write(fd_ndp_dev_a, (void *)ndp_ctrl, WRI_SIZE) == WRI_SIZE);
  //   do {
  //     assert(read(fd_ndp_dev_a, (void *)(ndp_ctrl + WRI_SIZE /
  //     sizeof(uint64_t)),
  //                 REA_SIZE) == REA_SIZE);
  //   } while (!pi_stat_rgst);
  // #else
  //   while (!pi_stat_rgst)
  //     ;
  // #endif
  //   res_hw = pi_last_rslt;
  //   end_hw = GET_TICKS;
  //
  //   printf("[%s] Strided_TSC:     sw: %6lu ns, hw: %6lu ns (norm: %2.3f)\n",
  //          res_sw == res_hw ? "PASS" : "FAIL", GET_ELAPS(start_sw, end_sw),
  //          GET_ELAPS(start_hw, end_hw),
  //          1.0 * GET_ELAPS(start_hw, end_hw) / GET_ELAPS(start_sw, end_sw));

  /* =========================== PointerChase_TSC ============================
   */
  auto start_sw = GET_TICKS;
  auto res_sw = pointer_chase(ndp_nodes, num_nodes, NDP_DATA);
  auto end_sw = GET_TICKS;

  // ---------------------------------------------------------
  // RUN 1: The Coherency Penalty (M -> S Downgrade)
  // ---------------------------------------------------------
  auto start_hw = GET_TICKS;
  pi_addr_data = NDP_DATA;
  pi_data_size = num_nodes;
  pi_cmmd_code = 4;
  pi_strt_rgst = START_CODE;
#if defined(FS) && defined(DRIVER)
  assert(write(fd_ndp_dev_a, (void *)ndp_ctrl, WRI_SIZE) == WRI_SIZE);
  do {
    assert(read(fd_ndp_dev_a, (void *)(ndp_ctrl + WRI_SIZE / sizeof(uint64_t)),
                REA_SIZE) == REA_SIZE);
  } while (!pi_stat_rgst);
#else
  while (!pi_stat_rgst)
    ;
#endif
  auto res_hw = pi_last_rslt;
  auto end_hw = GET_TICKS;

  printf("[%s] PntrChase (Dirty): sw: %6lu ns, hw: %6lu ns (norm: %2.3f)\n",
         res_sw == res_hw ? "PASS" : "FAIL", GET_ELAPS(start_sw, end_sw),
         GET_ELAPS(start_hw, end_hw),
         1.0 * GET_ELAPS(start_hw, end_hw) / GET_ELAPS(start_sw, end_sw));

  // ---------------------------------------------------------
  // RUN 2: The Isolated Baseline (Clean Read, No Snoops)
  // ---------------------------------------------------------
  auto start_hw_clean = GET_TICKS;
  pi_addr_data = NDP_DATA;
  pi_data_size = num_nodes;
  pi_cmmd_code = 4;
  pi_strt_rgst = START_CODE;
#if defined(FS) && defined(DRIVER)
  assert(write(fd_ndp_dev_a, (void *)ndp_ctrl, WRI_SIZE) == WRI_SIZE);
  do {
    assert(read(fd_ndp_dev_a, (void *)(ndp_ctrl + WRI_SIZE / sizeof(uint64_t)),
                REA_SIZE) == REA_SIZE);
  } while (!pi_stat_rgst);
#else
  while (!pi_stat_rgst)
    ;
#endif
  auto end_hw_clean = GET_TICKS;

  printf("[PASS] PntrChase (Clean):               hw: %6lu ns\n",
         GET_ELAPS(start_hw_clean, end_hw_clean));

  return 0;
}

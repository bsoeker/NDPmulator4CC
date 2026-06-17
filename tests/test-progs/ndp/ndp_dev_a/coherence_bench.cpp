#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#ifdef FS
#include <cassert>
#include <fcntl.h>
#include <sys/mman.h>
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
#define DATA_SIZE 0x100000
#define MAX_KEY (DATA_SIZE / 4)

#define GET_TICKS std::chrono::high_resolution_clock::now()
#define GET_ELAPS(A, B)                                                        \
  std::chrono::duration_cast<std::chrono::nanoseconds>(B - A).count()

int main(int argc, char *argv[]) {
  // -------------------------------------------------------------------------
  // 1. Initialization and Memory Mapping (Preserved from original framework)
  // -------------------------------------------------------------------------
  uint64_t *ndp_ctrl = (uint64_t *)NDP_CTRL;
  uint64_t *ndp_data = (uint64_t *)NDP_DATA;

  // =========================================================================
  // THE CACHE BUSTER SETUP: Randomized Linked List
  // =========================================================================
  std::cout << "Building Randomized Pointer-Chasing Graph..." << std::endl;

  std::vector<uint64_t> nodes;
  // A standard cache line is 64 bytes (8 uint64_t elements).
  // We step by 8 to guarantee every jump hits a completely new cache line.
  for (uint64_t i = 0; i < DATA_SIZE; i += 8) {
    nodes.push_back(i);
  }

  // Shuffle the nodes to destroy any physical spatial locality
  std::mt19937 rng(42); // Fixed seed for reproducible simulation timings
  std::shuffle(nodes.begin(), nodes.end(), rng);

  // Wire up the pointers in the physical memory array
  // ndp_data[index] will hold the pointer to the NEXT index.
  // ndp_data[index + 1] will hold the payload we mutate to trigger coherency.
  for (size_t i = 0; i < nodes.size() - 1; i++) {
    ndp_data[nodes[i]] = nodes[i + 1]; // Set next pointer
    ndp_data[nodes[i] + 1] = 0;        // Set initial payload
  }

  // Terminate the linked list with a definitive end marker (-1)
  ndp_data[nodes.back()] = (uint64_t)-1;
  uint64_t start_node = nodes[0];

  // Explicit target markers for the NDAcc search commands
  ndp_data[DATA_SIZE - 1] = MAX_KEY + 5;

  // Setup pointers to the memory-mapped Programmable IO registers
  uint64_t &pi_addr_data = ndp_ctrl[0];
  uint64_t &pi_data_size = ndp_ctrl[1];
  uint64_t &pi_data_skey = ndp_ctrl[2];
  uint64_t &pi_cmmd_code = ndp_ctrl[3];
  uint64_t &pi_strt_rgst = ndp_ctrl[4];
  uint64_t &pi_stat_rgst = ndp_ctrl[5];
  uint64_t &pi_last_rslt = ndp_ctrl[6];

  std::cout << "========== COHERENCE BENCHMARK STARTED ========" << std::endl;

  // -------------------------------------------------------------------------
  // EXPERIMENT 1: Host CPU vs. NDAcc Read/Write Contention (True Cache
  // Hammering)
  // -------------------------------------------------------------------------
  // Goal: Kick off an NDAcc linear scan while the CPU simultaneously overwrites
  // the exact same memory space. This forces cache invalidations or memory bus
  // locks.

  std::cout << "\n[EXP 1] Starting Read/Write Contention Run..." << std::endl;

  // Configure NDAcc to scan the whole array for a missing key (forces full
  // scan)
  pi_addr_data = NDP_DATA;
  pi_data_size = DATA_SIZE;
  pi_data_skey = MAX_KEY + 10; // Key that isn't present
  pi_cmmd_code = 0;            // Command 0: compare_n_hit

  auto start_exp1 = GET_TICKS;

  // FIRE THE ACCELERATOR (Asynchronous execution starts here)
  // pi_strt_rgst = START_CODE;

  // CONCURRENT HOST INTERFERENCE: The Pointer Chaser
  uint64_t cpu_write_count = 0;
  volatile uint64_t *v_ndp_data = (volatile uint64_t *)ndp_data;
  uint64_t curr = start_node;

  while (curr != (uint64_t)-1) {
    // 1. READ: Fetch the pointer to the next node (Guaranteed Cache Miss!)
    uint64_t next_idx = v_ndp_data[curr];

    // 2. WRITE: Mutate the payload (Triggers Coherency Invalidations)
    v_ndp_data[curr + 1] = v_ndp_data[curr + 1] + 1;

    // 3. JUMP: Move to the next random DRAM row
    curr = next_idx;
    cpu_write_count++;
  }

  // Polling Phase: Wait here for the accelerator to cross the finish line
  // while (!pi_stat_rgst)
  //   ; // Simple spin-lock loop over MMIO register

  auto end_exp1 = GET_TICKS;
  std::cout << "  -> EXP 1 Completed in: " << GET_ELAPS(start_exp1, end_exp1)
            << " ns" << std::endl;
  std::cout << "  -> Host CPU executed " << cpu_write_count
            << " concurrent updates during runtime." << std::endl;

  // -------------------------------------------------------------------------
  // EXPERIMENT 2: Coarse-Grained Parallel Working (True Data Sharing
  // Simulation)
  // -------------------------------------------------------------------------
  // Goal: Simulate a real hybrid processing pipeline. The array is split in
  // half. The CPU processes the upper half while the NDAcc processes the lower
  // half at the same time.

  std::cout << "\n[EXP 2] Starting Parallel Worker Split Run..." << std::endl;

  // Configure NDAcc to process the FIRST half of the dataset
  pi_addr_data = NDP_DATA;
  pi_data_size = DATA_SIZE / 2;
  pi_data_skey = MAX_KEY + 5;
  pi_cmmd_code = 0; // compare_n_hit

  auto start_exp2 = GET_TICKS;

  // FIRE THE ACCELERATOR (Asynchronous execution on Lower Half)
  // pi_strt_rgst = START_CODE;

  // CONCURRENT HOST PROCESSING:
  // CPU explicitly computes on the SECOND half of the data at the exact same
  // time. This will expose the overhead of spatial locality friction at the
  // boundary cache line.
  uint64_t cpu_discovered_hits = 0;
  uint64_t *cpu_half_ptr = ndp_data + (DATA_SIZE / 2);
  uint64_t cpu_half_size = DATA_SIZE / 2;

  cpu_discovered_hits = compare_n_hit(cpu_half_ptr, cpu_half_size, MAX_KEY + 5);

  // Polling Phase: Wait for the NDAcc to complete its half
  // while (!pi_stat_rgst)
  //   ;

  auto end_exp2 = GET_TICKS;
  uint64_t ndp_discovered_hits = pi_last_rslt;

  std::cout << "  -> EXP 2 Completed in: " << GET_ELAPS(start_exp2, end_exp2)
            << " ns" << std::endl;
  std::cout << "  -> Results - CPU Hit Status: " << cpu_discovered_hits
            << " | NDAcc Hit Status: " << ndp_discovered_hits << std::endl;

  return 0;
}

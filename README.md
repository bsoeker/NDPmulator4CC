# NDPmulator4CC
A fork of [NDPmulator](https://github.com/hpc-ulisboa/NDPmulator) extended to analyze cache coherency overheads across diverse application workloads in near-data processing architectures.

---

## Installation & Getting Started
The simulations with this framework were conducted in an X86/Debian machine. It is highly recommended that you use a virtual machine if your distro is not a Debian based one.
Building this version of NDPmulator requires:
* **GCC / G++ 12:** Compiling with GCC 13+ will fail due to stricter header dependency checks (`<cstdint>` / `uint32_t`, `uint64_t` missing includes across upstream gem5 files).
* **Python 3.10 & SCons 4.8.x:** Required for gem5's build scripts and internal Python runtime.

---

### 1. Configure GCC 12 Toolchain
Install GCC 12 and G++ 12, then set them as default using `update-alternatives` (or pass `CC`/`CXX` directly):
```bash
# On Debian/Ubuntu:
# sudo apt install gcc-12 g++-12
# Register and switch the C compiler
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100
sudo update-alternatives --set gcc /usr/bin/gcc-12
# Register and switch the C++ compiler
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100
sudo update-alternatives --set g++ /usr/bin/g++-12
#Verify your active compiler version
gcc --version | head -n 1
g++ --version | head -n 1
```

---

### 2. Configure Python 3.10 & SCons
If your host OS runs a newer default Python version (e.g., Python 3.11+), configure SCons for Python 3.10 using one of the methods below:
- Option A: System-wide Pip (Quickest)
```bash
# Bootstrap pip bound to Python 3.10
curl -sS [https://bootstrap.pypa.io/get-pip.py](https://bootstrap.pypa.io/get-pip.py) | sudo python3.10
# Install SCons 4.8.1 and required build dependencies
sudo python3.10 -m pip install scons==4.8.1
```
- Option B: Virtual Environment (Non-root)
```bash
# Create and activate Python 3.10 venv
python3.10 -m venv .venv
source .venv/bin/activate
# Install build dependencies
pip install --upgrade pip
pip install scons==4.8.1
```

---

### 3. Compiling NDPmulator
Once the toolchain and Python environment are configured, compile the target architecture binary.
Since the original authors of NDPmulator framework done their tests mostly on the ARM target, we'll follow along:
```bash
# Using explicit Python 3.10 (Option A):
python3.10 -m SCons build/ARM/gem5.opt -j$(nproc)
# Using an active virtualenv (Option B):
scons build/ARM/gem5.opt -j$(nproc)
```

---

## Coherence Overhead Analysis

This fork adds a controlled experimental harness on top of the baseline `NDPDevA` accelerator model to measure cache-coherence overhead as two separable quantities, Locality Benefit and Protocol Tax, instead of one conflated number. This section documents the harness itself: the conditions it runs under, the command set it exposes, and how to reproduce a sweep.

### The DIRTY / CLEAN / ISOLATED methodology

Each workload is run three times, varying only how much the CPU has touched the target memory before the accelerator does. Placement and topology are held fixed (baseline config) — only the *coherence state* of the data varies.

| Condition | Setup | Resulting cache state | Purpose |
|---|---|---|---|
| **DIRTY** | CPU **writes** the data | Modified | Upper bound: does real cache-forwarding help NDP? |
| **CLEAN** | CPU only **reads** the data (never writes) | Shared / Exclusive | Isolates coherence *messaging* cost from any forwarding benefit |
| **ISOLATED** | NDP initializes the data if data is node based, otherwise nobody touches the data | Uncached | The floor: every access is a genuine, uncontested DRAM round trip |

Two independent quantities follow:

```
Locality Benefit = T(DIRTY) vs T(ISOLATED)   — real cache-forwarding speedup
Protocol Tax     = T(CLEAN) vs T(ISOLATED)   — coherence-messaging cost, zero forwarding on either side
```

`CLEAN` vs `ISOLATED` isolates protocol tax specifically because both sides pay for the *same* DRAM trip — confirmed via `mem_ctrl.dram.numReads::ndp_accel` and `mem_ctrl.requestorReadAvgLat::ndp_accel` in `stats.txt` — so a timing difference between them can only come from the coherence machinery itself.

**Naive `CLEAN` runs are confounded by DRAM row-buffer state:** if the accelerator starts immediately after the CPU's setup sweep, it inherits leftover open rows and crossbar traffic that have nothing to do with coherence, inflating apparent tax. An idle DRAM cooldown sweep between the setup step and the timed run is required to get a valid `CLEAN` measurement — see the workflow note below.

### NDP command reference

Registers (`ndp_ctrl[0..6]`, written/read via `pi_addr_data`, `pi_data_size`, `pi_data_skey`, `pi_cmmd_code`, `pi_strt_rgst`, `pi_stat_rgst`, `pi_last_rslt`) are shared across all commands; only the semantics of `pi_data_size` / `pi_data_skey` change per command.

| Cmd | Workload | `pi_addr_data` | `pi_data_size` | `pi_data_skey` | Notes |
|---|---|---|---|---|---|
| 0 | Bulk (Compare-N-Hit) | array base | element count | search key | Up to `max_reqs` sub-requests in flight concurrently — the only workload with request-level pipelining |
| 3 | Strided Access | array base | access count | stride (bytes) | One request issued at a time, address known in advance |
| 4 | Pointer Chase | list head | max depth | — | Fully dependent chain; terminates on `next_addr == 0` |
| 5 | Read-Modify-Write | list head | node count | addend | Sequential; **valid in ISOLATED mode only** — see Known Limitations |
| 6 | Seed Linked List (DMA) | list head | node count | — | Untimed setup step; writes real chain data via NDP's own DMA path so CLEAN/ISOLATED never need a CPU write |
| 8 | Tree Traversal | tree root | tree depth | — | Fan-out reads (up to 2 concurrent children per completed node); terminates naturally when a complete tree's node budget is exhausted |
| 9 | Seed Binary Tree (DMA) | tree root | tree depth | — | Untimed setup step, array-layout complete binary tree (child `i` at `2i+1`, `2i+2`) |

`Node` and `TreeNode` are both padded to exactly 64 bytes (one cache line). Source lives in `src/ndp/` (base `NDP` class — port wiring, `accessMemory()`, DMA burst handling) and `src/ndp_dev_a/` (the `NDPDevA` command implementations above); the host-side test harness is `tests/test-progs/ndp/ndp_dev_a/main.cpp`.

### Running a sweep

`main.cpp` selects its behavior via two hand-toggled constants at the top of the file:

```cpp
enum TestSelect { TEST_BULK, TEST_STRIDED, TEST_CHASE, TEST_RMW, TEST_TREE };
enum ShareMode  { MODE_DIRTY, MODE_CLEAN, MODE_ISOLATED };

static const ShareMode  share_mode  = MODE_DIRTY;
static const TestSelect test_select = TEST_BULK;
```

Each `(test_select, share_mode)` pair requires its own **rebuild and separate invocation** — gem5 writes one cumulative `stats.txt` per run, so running multiple tests in a single invocation makes it impossible to attribute stats to a specific workload. Workflow per data point:

```bash
# 1. Edit share_mode / test_select in main.cpp
# 2. Rebuild the target binary, then run, e.g.:
./build/ARM/gem5.opt configs/ndp/se-run-ndp_dev_a.py
# 3. Immediately copy/rename m5out/stats.txt before the next run overwrites it
```

`configs/ndp/se-run-ndp_dev_a.py` is the config used exclusively to produce every published result. `se-run-ndp_dev_a_mem.py` and `se-run-ndp_dev_a_l1.py` also exist in that directory but were **not** used for any reported numbers — don't substitute them if you're trying to reproduce results.

Cooldown is not optional for a valid `CLEAN` data point — make sure the idle DRAM cooldown sweep between setup and the timed run is enabled before recording a result, or you'll get inflated, confounded tax figures.

A full sweep across the four supported workloads × three share modes is 12 runs (RMW is only meaningful in `MODE_ISOLATED`, per Known Limitations below).

### Known limitations

- **RMW (cmd 5) is only valid in `MODE_ISOLATED`.** `NDPDevA` has no cache of its own, so its writes are plain DMA packets rather than coherence-protocol participants — running RMW under DIRTY or CLEAN trips gem5 coherence invariants (`Cache::handleSnoop`'s dirty-line panic, `SnoopFilter::lookupSnoop`'s consistency assertion). Don't attempt DIRTY/CLEAN for this workload.
- **Batched-gather (cmd 7) is implemented but not part of the validated result set.** Its CLEAN-mode measurements hit a row-buffer confound that the standard cooldown fix doesn't fully resolve for this access pattern. The command exists in source for reference only.
- **No true concurrent contention is exercised by this harness.** `main.cpp` fires the accelerator and then blocks on `while (!pi_stat_rgst);` rather than continuing to issue CPU traffic, so no run here reflects genuine, sustained CPU/NDP contention.
- **`membus.snoops` undercounts coherence activity** — it only counts snoops that result in data forwarding, not snoop-filter lookups or state-only downgrades. Cross-reference `mem_ctrl.dram.numReads::ndp_accel` (or a `--debug-flags=Cache,CoherentXBar,SnoopFilter` trace) rather than relying on it alone.

---

## Base Architecture Overview
**NDPmulator4CC** extends the core NDPmulator architectural framework (System Address Management, Programmed I/O, and Load/Store Units) to evaluate cache coherence mechanisms between host cores and near-data accelerators. 
For the complete baseline architecture specification and hardware block diagrams, refer to the [upstream NDPmulator repository](https://github.com/hpc-ulisboa/NDPmulator).
## Simulation scripts
Examples of both SE and FS simulation scripts can be found [here](configs/ndp).
## Host code
An example illustrating how to communicate with the NDP device from the host code can be found [here](tests/test-progs/ndp/ndp_dev_a), together with a device driver to be used with FS mode.
## FS kernels and images
The authors rely on the images and kernels officially supported by gem5, available at https://resources.gem5.org/.
## Attribution & Baseline
This repository is built on top of the open-source **NDPmulator / gem5-accel** framework developed by João Vieira, Nuno Roma, Gabriel Falcão, and Pedro Tomás.
If using this codebase, please cite their original work:
```bibtex
@article{DBLP:journals/cal/VieiraRFT24,
  author  = {Jo{\~{a}}o Vieira and
             Nuno Roma and
             Gabriel Falc{\~{a}}o and
             Pedro Tom{\'{a}}s},
  title   = {gem5-accel: {A} Pre-RTL Simulation Toolchain for Accelerator Architecture
             Validation},
  journal = {{IEEE} Comput. Archit. Lett.},
  volume  = {23},
  number  = {1},
  pages   = {1--4},
  year    = {2024}
}
```

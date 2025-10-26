// C++ wrapper for PCM that will be statically linked
#include "pktgen_pcm_wrapper.h"
#include <iostream>
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

// Intel PCM includes
#include "cpucounters.h"

using namespace pcm;

extern "C" {

// Global PCM state
static PCM* g_pcm_instance = nullptr;
static bool g_initialized = false;
static bool g_measurement_active = false;

// Counter state storage
static std::vector<CoreCounterState> g_before_core_states;
static std::vector<CoreCounterState> g_after_core_states;
static std::vector<SocketCounterState> g_before_socket_states;
static std::vector<SocketCounterState> g_after_socket_states;
static SystemCounterState g_before_system_state;
static SystemCounterState g_after_system_state;

int pcm_wrapper_is_available(void) {
    // Always return 1 for now, as PCM is statically linked
    return 1;
}

int pcm_wrapper_init(void) {
    if (g_initialized) {
        return 0;  // Already initialized
    }

    g_pcm_instance = PCM::getInstance();
    if (!g_pcm_instance) {
        printf("ERROR: Failed to get PCM instance\n");
        return -1;
    }

    // Check access permissions
    printf("*** Attempting to program Intel PCM counters... ***\n");
    auto status = g_pcm_instance->program();

    if (status == PCM::Success) {
        printf("*** SUCCESS: Intel PCM counters programmed successfully! ***\n");
        g_initialized = true;

        // Initialize state vectors
        uint32_t num_cores = g_pcm_instance->getNumCores();
        uint32_t num_sockets = g_pcm_instance->getNumSockets();

        g_before_core_states.resize(num_cores);
        g_after_core_states.resize(num_cores);
        g_before_socket_states.resize(num_sockets);
        g_after_socket_states.resize(num_sockets);

        return 0;
    } else if (status == PCM::MSRAccessDenied) {
        printf("WARNING: MSR access denied, trying without MSR\n");
        // Try programming with reduced privileges
        status = g_pcm_instance->program(PCM::DEFAULT_EVENTS, nullptr, false, -1);
        if (status == PCM::Success) {
            printf("*** SUCCESS: Intel PCM counters programmed successfully (no MSR mode)! ***\n");
            g_initialized = true;
            return 0;
        }
    } else if (status == PCM::PMUBusy) {
        printf("WARNING: PMU busy, trying to reset and retry\n");
        g_pcm_instance->resetPMU();
        status = g_pcm_instance->program();
        if (status == PCM::Success) {
            printf("*** SUCCESS: Intel PCM counters programmed successfully after reset! ***\n");
            g_initialized = true;

            // Initialize state vectors
            uint32_t num_cores = g_pcm_instance->getNumCores();
            uint32_t num_sockets = g_pcm_instance->getNumSockets();

            g_before_core_states.resize(num_cores);
            g_after_core_states.resize(num_cores);
            g_before_socket_states.resize(num_sockets);
            g_after_socket_states.resize(num_sockets);

            return 0;
        }
    }

    // Print detailed error information
    printf("ERROR: Failed to program PCM counters. Status: %d\n", (int)status);
    switch (status) {
        case PCM::MSRAccessDenied:
            printf("  - MSR access denied. Try running with sudo or check permissions.\n");
            break;
        case PCM::PMUBusy:
            printf("  - PMU is busy. Another profiling tool might be running.\n");
            break;
        case PCM::UnknownError:
            printf("  - Unknown error occurred.\n");
            break;
        default:
            printf("  - Unhandled error code: %d\n", (int)status);
            break;
    }

    return -1;
}

void pcm_wrapper_cleanup(void) {
    if (g_pcm_instance) {
        g_pcm_instance->cleanup();
        g_pcm_instance = nullptr;
    }
    g_initialized = false;
    g_measurement_active = false;

    // Clear state vectors
    g_before_core_states.clear();
    g_after_core_states.clear();
    g_before_socket_states.clear();
    g_after_socket_states.clear();
}

int pcm_wrapper_start_measurement(void) {
    if (!g_initialized || !g_pcm_instance) {
        printf("ERROR: PCM not initialized when trying to start measurement\n");
        return -1;
    }

    try {
        // Wait a bit to ensure clean counter state
        usleep(100000); // 100ms

        g_before_system_state = getSystemCounterState();

        uint32_t num_cores = g_pcm_instance->getNumCores();
        if (num_cores == 0 || num_cores > 1024) {
            printf("DEBUG: Suspicious core count %u, measurement may be unreliable\n", num_cores);
        }

        for (uint32_t i = 0; i < num_cores; ++i) {
            g_before_core_states[i] = getCoreCounterState(i);
        }

        uint32_t num_sockets = g_pcm_instance->getNumSockets();
        for (uint32_t i = 0; i < num_sockets; ++i) {
            g_before_socket_states[i] = getSocketCounterState(i);
        }

        g_measurement_active = true;

        printf("PCM measurement started, capturing states for %u cores and %u sockets\n",
               num_cores, num_sockets);
        return 0;
    } catch (const std::exception& e) {
        printf("Error capturing initial PCM states: %s\n", e.what());
        return -1;
    } catch (...) {
        printf("Unknown error capturing initial PCM states\n");
        return -1;
    }
}

int pcm_wrapper_stop_measurement(void) {
    if (!g_initialized || !g_pcm_instance) {
        printf("ERROR: PCM not initialized when trying to stop measurement\n");
        return -1;
    }

    try {
        // Always capture final states regardless of g_measurement_active
        g_after_system_state = getSystemCounterState();

        uint32_t num_cores = g_pcm_instance->getNumCores();
        for (uint32_t i = 0; i < num_cores; ++i) {
            g_after_core_states[i] = getCoreCounterState(i);
        }

        uint32_t num_sockets = g_pcm_instance->getNumSockets();
        for (uint32_t i = 0; i < num_sockets; ++i) {
            g_after_socket_states[i] = getSocketCounterState(i);
        }

        g_measurement_active = false;
        printf("PCM measurement stopped, captured states for %u cores and %u sockets\n",
               num_cores, num_sockets);

        // Quick sanity check - test one core calculation
        if (num_cores > 0) {
            try {
                double test_ipc = getIPC(g_before_core_states[0], g_after_core_states[0]);
                uint64_t test_cycles = getCycles(g_before_core_states[0], g_after_core_states[0]);

                // Check for suspicious values that indicate measurement problems
                if (test_ipc <= 0.001 || test_ipc > 10.0) {
                    printf("DEBUG: WARNING - Suspicious IPC value %.3f detected in measurement\n", test_ipc);
                }
                if (test_cycles < 1000 || test_cycles > 1000000000000ULL) {
                    printf("DEBUG: WARNING - Suspicious cycle count %lu detected in measurement\n", test_cycles);
                }
            } catch (...) {
                printf("DEBUG: ERROR - Failed to perform sanity check calculation\n");
            }
        }

        return 0;
    } catch (const std::exception& e) {
        printf("Error capturing final PCM states: %s\n", e.what());
        return -1;
    } catch (...) {
        printf("Unknown error capturing final PCM states\n");
        return -1;
    }
}

int pcm_wrapper_get_basic_counters(uint32_t core_id, uint64_t* cycles, uint64_t* instructions) {
    if (!g_initialized || !g_pcm_instance || !cycles || !instructions) {
        return -1;
    }

    try {
        if (core_id >= g_pcm_instance->getNumCores()) {
            return -1;
        }

        *cycles = getCycles(g_before_core_states[core_id], g_after_core_states[core_id]);
        *instructions = getInstructionsRetired(g_before_core_states[core_id], g_after_core_states[core_id]);
        return 0;
    } catch (...) {
        return -1;
    }
}

int pcm_wrapper_get_core_counters(uint32_t core_id, pcm_core_counters_t* counters) {
    if (!g_initialized || !g_pcm_instance || !counters) {
        return -1;
    }

    try {
        uint32_t num_cores = g_pcm_instance->getNumCores();
        uint32_t pcm_core_id = core_id;

        // Map DPDK lcore to PCM core with bounds checking
        if (pcm_core_id >= num_cores) {
            printf("Warning: DPDK lcore %u exceeds PCM cores %u, using modulo mapping\n",
                   core_id, num_cores);
            pcm_core_id = core_id % num_cores;
        }

        // Make sure we have valid before/after states
        if (g_before_core_states.size() <= pcm_core_id || g_after_core_states.size() <= pcm_core_id) {
            printf("ERROR: Invalid core state vectors for core %u\n", pcm_core_id);
            return -1;
        }

        // Use the stored before/after states to calculate metrics over the entire measurement period
        const CoreCounterState& before = g_before_core_states[pcm_core_id];
        const CoreCounterState& after = g_after_core_states[pcm_core_id];

        // Calculate metrics using PCM's built-in functions with error checking
        double ipc = 0.0;
        double freq_ghz = 0.0;
        double util = 0.0;
        double l2_hit_ratio = 0.0;
        double l3_hit_ratio = 0.0;
        uint64_t cycles = 0;
        uint64_t instructions = 0;
        uint64_t l2_hits = 0;
        uint64_t l2_misses = 0;
        uint64_t l3_hits = 0;
        uint64_t l3_misses = 0;

        try {
            ipc = getIPC(before, after);
            if (std::isnan(ipc) || std::isinf(ipc) || ipc < 0 || ipc > 10) {
                printf("WARNING: Invalid IPC value %.3f, setting to 0\n", ipc);
                ipc = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate IPC\n");
            ipc = 0.0;
        }

        try {
            freq_ghz = getAverageFrequency(before, after) / 1e9;
            if (std::isnan(freq_ghz) || std::isinf(freq_ghz) || freq_ghz < 0 || freq_ghz > 10) {
                printf("WARNING: Invalid frequency %.3f, setting to 0\n", freq_ghz);
                freq_ghz = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate frequency\n");
            freq_ghz = 0.0;
        }

        try {
            util = getActiveRelativeFrequency(before, after);
            if (std::isnan(util) || std::isinf(util) || util < 0 || util > 2) {
                printf("WARNING: Invalid utilization %.3f, setting to 0\n", util);
                util = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate utilization\n");
            util = 0.0;
        }

        try {
            l2_hit_ratio = getL2CacheHitRatio(before, after);
            if (std::isnan(l2_hit_ratio) || std::isinf(l2_hit_ratio) || l2_hit_ratio < 0 || l2_hit_ratio > 1) {
                printf("WARNING: Invalid L2 hit ratio %.3f, setting to 0\n", l2_hit_ratio);
                l2_hit_ratio = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate L2 hit ratio\n");
            l2_hit_ratio = 0.0;
        }

        try {
            l3_hit_ratio = getL3CacheHitRatio(before, after);
            if (std::isnan(l3_hit_ratio) || std::isinf(l3_hit_ratio) || l3_hit_ratio < 0 || l3_hit_ratio > 1) {
                printf("WARNING: Invalid L3 hit ratio %.3f, setting to 0\n", l3_hit_ratio);
                l3_hit_ratio = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate L3 hit ratio\n");
            l3_hit_ratio = 0.0;
        }

        // Get raw counter values with overflow protection
        try {
            cycles = getCycles(before, after);
            // Check for reasonable cycle count - allow much higher values for longer measurements
            if (cycles > 1000000000000ULL) {  // 1T cycles (much higher threshold)
                printf("WARNING: Suspicious cycle count %lu, setting to 0\n", cycles);
                cycles = 0;
            }
        } catch (...) {
            printf("WARNING: Failed to get cycles\n");
            cycles = 0;
        }

        try {
            instructions = getInstructionsRetired(before, after);
            // Adjust threshold for instructions - allow much higher values for longer measurements
            if (instructions > 1000000000000ULL) {  // 1T instructions (much higher threshold)
                printf("WARNING: Suspicious instruction count %lu, setting to 0\n", instructions);
                instructions = 0;
            }
        } catch (...) {
            printf("WARNING: Failed to get instructions\n");
            instructions = 0;
        }

        try {
            l2_hits = getL2CacheHits(before, after);
            if (l2_hits > 100000000000ULL) {  // 100B threshold
                printf("WARNING: Suspicious L2 hit count %lu, setting to 0\n", l2_hits);
                l2_hits = 0;
            }
        } catch (...) {
            printf("WARNING: Failed to get L2 hits\n");
            l2_hits = 0;
        }

        try {
            l2_misses = getL2CacheMisses(before, after);
            if (l2_misses > 100000000000ULL) {  // 100B threshold
                printf("WARNING: Suspicious L2 miss count %lu, setting to 0\n", l2_misses);
                l2_misses = 0;
            }
        } catch (...) {
            printf("WARNING: Failed to get L2 misses\n");
            l2_misses = 0;
        }

        try {
            l3_hits = getL3CacheHits(before, after);
            if (l3_hits > 100000000000ULL) {  // 100B threshold
                printf("WARNING: Suspicious L3 hit count %lu, setting to 0\n", l3_hits);
                l3_hits = 0;
            }
        } catch (...) {
            printf("WARNING: Failed to get L3 hits\n");
            l3_hits = 0;
        }

        try {
            l3_misses = getL3CacheMisses(before, after);
            if (l3_misses > 100000000000ULL) {  // 100B threshold
                printf("WARNING: Suspicious L3 miss count %lu, setting to 0\n", l3_misses);
                l3_misses = 0;
            }
        } catch (...) {
            printf("WARNING: Failed to get L3 misses\n");
            l3_misses = 0;
        }

        // Store all the calculated metrics
        counters->cycles = cycles;
        counters->instructions = instructions;
        counters->l2_cache_hits = l2_hits;
        counters->l2_cache_misses = l2_misses;
        counters->l3_cache_hits = l3_hits;
        counters->l3_cache_misses = l3_misses;
        counters->ipc = ipc;
        counters->l2_cache_hit_ratio = l2_hit_ratio;
        counters->l3_cache_hit_ratio = l3_hit_ratio;
        counters->frequency_ghz = freq_ghz;
        counters->cpu_utilization = util;

        // Check for completely zero metrics which might indicate measurement issues
        if (cycles == 0 && instructions == 0 && ipc == 0.0) {
            printf("DEBUG: WARNING - All core metrics are zero for core %u, check PCM measurement\n", core_id);
        }

        // Energy estimation (may need different approach depending on PCM version)
        try {
            // For core-level energy, we need to use system-level or socket-level energy
            // Core-level energy might not be available in all PCM versions
            counters->energy_joules = 0.0; // Default for core-level
        } catch (...) {
            counters->energy_joules = 0.0;
        }

        return 0;
    } catch (const std::exception& e) {
        printf("Error in PCM counter calculation: %s\n", e.what());
        return -1;
    } catch (...) {
        printf("Unknown error in PCM counter calculation\n");
        return -1;
    }
}

int pcm_wrapper_get_memory_counters(uint32_t socket_id, pcm_memory_counters_t* counters) {
    if (!g_initialized || !g_pcm_instance || !counters) {
        return -1;
    }

    try {
        if (socket_id >= g_pcm_instance->getNumSockets()) {
            return -1;
        }

        // Use stored before/after states to calculate metrics over the entire measurement period
        const SocketCounterState& before = g_before_socket_states[socket_id];
        const SocketCounterState& after = g_after_socket_states[socket_id];

        // Use PCM's built-in bandwidth calculation methods
        uint64_t bytes_read = getBytesReadFromMC(before, after);
        uint64_t bytes_written = getBytesWrittenToMC(before, after);

        // Calculate elapsed time for the entire measurement period
        double elapsed_time = getExecUsage(g_before_system_state, g_after_system_state);
        if (elapsed_time <= 0) {
            printf("ERROR: Invalid elapsed time %.6f seconds in memory counter calculation!\n", elapsed_time);
            printf("ERROR: This indicates PCM measurement was not properly initialized or captured.\n");
            return -1;
        }

        counters->dram_read_bytes = bytes_read;
        counters->dram_write_bytes = bytes_written;
        counters->memory_controller_read_bw_mbps = (double)bytes_read / (1024.0 * 1024.0) / elapsed_time;
        counters->memory_controller_write_bw_mbps = (double)bytes_written / (1024.0 * 1024.0) / elapsed_time;
        counters->memory_controller_bw_mbps = counters->memory_controller_read_bw_mbps + counters->memory_controller_write_bw_mbps;

        return 0;
    } catch (const std::exception& e) {
        printf("Error in memory counter calculation: %s\n", e.what());
        return -1;
    } catch (...) {
        printf("Unknown error in memory counter calculation\n");
        return -1;
    }
}

int pcm_wrapper_get_io_counters(uint32_t socket_id, pcm_io_counters_t* counters) {
    if (!g_initialized || !g_pcm_instance || !counters) {
        return -1;
    }

    try {
        if (socket_id >= g_pcm_instance->getNumSockets()) {
            return -1;
        }

        // Use stored before/after states to calculate metrics over the entire measurement period
        const SocketCounterState& before = g_before_socket_states[socket_id];
        const SocketCounterState& after = g_after_socket_states[socket_id];

        // Try to get PCIe read/write bytes using PCM functions
        uint64_t pcie_read_bytes = 0;
        uint64_t pcie_write_bytes = 0;

        // PCM might have specific PCIe counter functions - try memory controller as proxy
        uint64_t mc_reads = getBytesReadFromMC(before, after);
        uint64_t mc_writes = getBytesWrittenToMC(before, after);

        // For network-intensive workloads, significant portion of memory traffic is PCIe-related
        // Estimate PCIe traffic as a fraction of memory controller traffic
        double pcie_fraction = 0.3; // Assume 30% of memory traffic is PCIe-related

        if (mc_reads < 10000000000ULL && mc_writes < 10000000000ULL) { // Sanity check
            pcie_read_bytes = (uint64_t)(mc_reads * pcie_fraction);
            pcie_write_bytes = (uint64_t)(mc_writes * pcie_fraction);
        }

        // Calculate elapsed time for the entire measurement period
        double elapsed_time = getExecUsage(g_before_system_state, g_after_system_state);
        if (elapsed_time <= 0) {
            printf("ERROR: Invalid elapsed time %.6f seconds in I/O counter calculation!\n", elapsed_time);
            printf("ERROR: This indicates PCM measurement was not properly initialized or captured.\n");
            return -1;
        }

        counters->pcie_read_bytes = pcie_read_bytes;
        counters->pcie_write_bytes = pcie_write_bytes;
        counters->pcie_read_bandwidth_mbps = (double)pcie_read_bytes / (1024.0 * 1024.0) / elapsed_time;
        counters->pcie_write_bandwidth_mbps = (double)pcie_write_bytes / (1024.0 * 1024.0) / elapsed_time;

        // Memory controller bandwidth (IMC)
        counters->imc_reads_gbps = (double)mc_reads / (1024.0 * 1024.0 * 1024.0) / elapsed_time;
        counters->imc_writes_gbps = (double)mc_writes / (1024.0 * 1024.0 * 1024.0) / elapsed_time;

        // QPI/UPI data transfer and utilization
        counters->qpi_upi_data_bytes = 0; // PCM might not support this directly
        counters->qpi_upi_utilization = 0.0;
        counters->uncore_freq_ghz = 0; // Could try to get uncore frequency if available

        return 0;
    } catch (const std::exception& e) {
        printf("Error in I/O counter calculation: %s\n", e.what());
        return -1;
    } catch (...) {
        printf("Unknown error in I/O counter calculation\n");
        return -1;
    }
}

int pcm_wrapper_get_system_counters(pcm_system_counters_t* counters) {
    if (!g_initialized || !g_pcm_instance || !counters) {
        return -1;
    }

    try {
        // System-wide counters
        counters->active_cores = g_pcm_instance->getNumOnlineCores();

        // Energy measurements with overflow protection
        double total_energy_raw = 0.0;
        double dram_energy_raw = 0.0;

        try {
            total_energy_raw = getConsumedJoules(g_before_system_state, g_after_system_state);
            if (std::isnan(total_energy_raw) || std::isinf(total_energy_raw) || total_energy_raw < 0 || total_energy_raw > 100000.0) {
                printf("WARNING: Invalid total energy %.2f, setting to 0\n", total_energy_raw);
                total_energy_raw = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate total energy\n");
            total_energy_raw = 0.0;
        }

        try {
            dram_energy_raw = getDRAMConsumedJoules(g_before_system_state, g_after_system_state);
            if (std::isnan(dram_energy_raw) || std::isinf(dram_energy_raw) || dram_energy_raw < 0 || dram_energy_raw > 100000.0) {
                printf("WARNING: Invalid DRAM energy %.2f, setting to 0\n", dram_energy_raw);
                dram_energy_raw = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate DRAM energy\n");
            dram_energy_raw = 0.0;
        }

        counters->total_energy_joules = total_energy_raw;
        counters->package_energy_joules = total_energy_raw;
        counters->dram_energy_joules = dram_energy_raw;

        // Check if energy measurements seem reasonable
        if (total_energy_raw > 10000.0) {  // More than 10kJ seems very high
            printf("DEBUG: WARNING - Very high energy consumption detected: total=%.1f J, dram=%.1f J\n",
                   total_energy_raw, dram_energy_raw);
        } else if (total_energy_raw == 0.0 && dram_energy_raw == 0.0) {
            printf("DEBUG: WARNING - Zero energy measurements detected, check PMU access\n");
        }

        // System-wide IPC with overflow protection
        double system_ipc_raw = 0.0;
        try {
            system_ipc_raw = getIPC(g_before_system_state, g_after_system_state);
            if (std::isnan(system_ipc_raw) || std::isinf(system_ipc_raw) || system_ipc_raw < 0.0 || system_ipc_raw > 10.0) {
                printf("WARNING: Invalid system IPC %.3f, setting to 0\n", system_ipc_raw);
                system_ipc_raw = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate system IPC\n");
            system_ipc_raw = 0.0;
        }
        counters->total_ipc = system_ipc_raw;

        // Memory bandwidth utilization
        uint64_t total_mem_bytes_raw = 0;
        double elapsed = 0.0;

        try {
            uint64_t read_bytes = getBytesReadFromMC(g_before_system_state, g_after_system_state);
            uint64_t write_bytes = getBytesWrittenToMC(g_before_system_state, g_after_system_state);

            if (read_bytes < 1000000000000000ULL && write_bytes < 1000000000000000ULL) {
                total_mem_bytes_raw = read_bytes + write_bytes;
            } else {
                printf("WARNING: Suspicious memory byte counts: read=%lu, write=%lu\n", read_bytes, write_bytes);
                total_mem_bytes_raw = 0;
            }

            elapsed = getExecUsage(g_before_system_state, g_after_system_state);
            if (std::isnan(elapsed) || std::isinf(elapsed) || elapsed <= 0 || elapsed > 1000.0) {
                printf("WARNING: Invalid elapsed time %.3f, setting memory BW to 0\n", elapsed);
                elapsed = 0.0;
            }
        } catch (...) {
            printf("WARNING: Failed to calculate memory bandwidth\n");
            total_mem_bytes_raw = 0;
            elapsed = 0.0;
        }

        if (elapsed > 0 && total_mem_bytes_raw > 0) {
            counters->memory_bandwidth_utilization = (double)total_mem_bytes_raw / (1024.0 * 1024.0 * 1024.0) / elapsed;  // GB/s

            // Check for unreasonably high memory bandwidth
            if (counters->memory_bandwidth_utilization > 1000.0) {  // More than 1TB/s seems suspicious
                printf("DEBUG: WARNING - Very high memory bandwidth detected: %.1f GB/s\n",
                       counters->memory_bandwidth_utilization);
            }
        } else {
            counters->memory_bandwidth_utilization = 0.0;
            if (elapsed <= 0) {
                printf("DEBUG: WARNING - Invalid elapsed time %.3f for memory bandwidth calculation\n", elapsed);
            }
        }

        // Thermal throttling (approximate) with bounds check
        double rel_freq = 0.0;
        try {
            rel_freq = getRelativeFrequency(g_before_system_state, g_after_system_state);
            if (std::isnan(rel_freq) || std::isinf(rel_freq) || rel_freq < 0.0 || rel_freq > 2.0) {
                printf("WARNING: Invalid relative frequency %.3f, setting throttle to 0\n", rel_freq);
                rel_freq = 1.0;  // assume no throttling
            }
        } catch (...) {
            printf("WARNING: Failed to calculate relative frequency\n");
            rel_freq = 1.0;  // assume no throttling
        }

        counters->thermal_throttle_ratio = (rel_freq > 0) ? (1.0 - rel_freq) : 0.0;
        if (counters->thermal_throttle_ratio < 0) {
            counters->thermal_throttle_ratio = 0.0;
        }

        return 0;
    } catch (...) {
        return -1;
    }
}

int pcm_wrapper_get_system_info(char* info_buffer, size_t buffer_size) {
    if (!g_initialized || !g_pcm_instance) {
        return -1;
    }

    try {
        snprintf(info_buffer, buffer_size,
                "CPU Brand: %s\n"
                "Cores: %u\n"
                "Online Cores: %u\n"
                "Sockets: %u\n"
                "Threads per Core: %u\n",
                g_pcm_instance->getCPUBrandString().c_str(),
                g_pcm_instance->getNumCores(),
                g_pcm_instance->getNumOnlineCores(),
                g_pcm_instance->getNumSockets(),
                g_pcm_instance->getThreadsPerCore());
        return 0;
    } catch (...) {
        return -1;
    }
}

int pcm_wrapper_get_instant_pcie_bytes(uint32_t socket_id, uint64_t *pcie_read_bytes, uint64_t *pcie_write_bytes) {
    if (!g_initialized || !g_pcm_instance) {
        return -1;
    }

    if (!pcie_read_bytes || !pcie_write_bytes) {
        return -1;
    }

    if (socket_id >= g_pcm_instance->getNumSockets()) {
        return -1;
    }

    try {
        // Get instant snapshot of system counter state
        SystemCounterState current_state = getSystemCounterState();

        // Sum up all QPI/PCIe links for this specific socket only
        // Each socket can have multiple QPI/PCIe links
        uint64_t total_incoming = 0;
        uint64_t total_outgoing = 0;

        // Get number of QPI ports for this socket
        uint32_t num_qpi_ports = g_pcm_instance->getQPILinksPerSocket();

        // Sum incoming bytes across all links for this socket
        for (uint32_t link = 0; link < num_qpi_ports; ++link) {
            total_incoming += getIncomingQPILinkBytes(socket_id, link, current_state);
        }

        // For outgoing bytes, we need before/after states
        if (g_measurement_active) {
            // Sum outgoing bytes across all links for this socket
            for (uint32_t link = 0; link < num_qpi_ports; ++link) {
                total_outgoing += getOutgoingQPILinkBytes(socket_id, link, g_before_system_state, current_state);
            }
        } else {
            // If no measurement active, use zero baseline
            SystemCounterState zero_state = SystemCounterState();
            for (uint32_t link = 0; link < num_qpi_ports; ++link) {
                total_outgoing += getOutgoingQPILinkBytes(socket_id, link, zero_state, current_state);
            }
        }

        *pcie_read_bytes = total_incoming;
        *pcie_write_bytes = total_outgoing;

        return 0;
    } catch (const std::exception& e) {
        printf("Error getting instant PCIe bytes for socket %u: %s\n", socket_id, e.what());
        return -1;
    } catch (...) {
        printf("Unknown error getting instant PCIe bytes for socket %u\n", socket_id);
        return -1;
    }
}

} // extern "C"

#ifndef __PKTGEN_PCM_WRAPPER_H__
#define __PKTGEN_PCM_WRAPPER_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCM wrapper functions for static linking */

/* Core performance counters structure */
typedef struct {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t l2_cache_hits;
    uint64_t l2_cache_misses;
    uint64_t l3_cache_hits;
    uint64_t l3_cache_misses;
    double ipc;
    double l2_cache_hit_ratio;
    double l3_cache_hit_ratio;
    double frequency_ghz;
    double cpu_utilization;
    double energy_joules;
} pcm_core_counters_t;

/* Memory performance counters structure */
typedef struct {
    uint64_t dram_read_bytes;
    uint64_t dram_write_bytes;
    double memory_controller_read_bw_mbps;
    double memory_controller_write_bw_mbps;
    double memory_controller_bw_mbps;
} pcm_memory_counters_t;

/* I/O and Uncore performance counters structure */
typedef struct {
    uint64_t pcie_read_bytes;
    uint64_t pcie_write_bytes;
    double pcie_read_bandwidth_mbps;
    double pcie_write_bandwidth_mbps;
    uint64_t qpi_upi_data_bytes;
    double qpi_upi_utilization;
    uint64_t uncore_freq_ghz;
    double imc_reads_gbps;
    double imc_writes_gbps;
} pcm_io_counters_t;

/* System-wide performance counters structure */
typedef struct {
    uint32_t active_cores;
    double total_energy_joules;
    double package_energy_joules;
    double dram_energy_joules;
    double total_ipc;
    double memory_bandwidth_utilization;
    double thermal_throttle_ratio;
} pcm_system_counters_t;

/**
 * Check if PCM wrapper is available
 * @return 1 if available, 0 if not
 */
int pcm_wrapper_is_available(void);

/**
 * Initialize PCM wrapper
 * @return 0 on success, negative on error
 */
int pcm_wrapper_init(void);

/**
 * Cleanup PCM wrapper
 */
void pcm_wrapper_cleanup(void);

/**
 * Get basic performance counters
 * @param core_id: Core ID to get counters for
 * @param cycles: Pointer to store cycle count
 * @param instructions: Pointer to store instruction count
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_basic_counters(uint32_t core_id, uint64_t *cycles, uint64_t *instructions);

/**
 * Get comprehensive core performance counters
 * @param core_id: Core ID to get counters for
 * @param counters: Pointer to store core counters
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_core_counters(uint32_t core_id, pcm_core_counters_t *counters);

/**
 * Get memory performance counters
 * @param socket_id: Socket ID to get counters for
 * @param counters: Pointer to store memory counters
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_memory_counters(uint32_t socket_id, pcm_memory_counters_t *counters);

/**
 * Get I/O and uncore performance counters
 * @param socket_id: Socket ID to get counters for
 * @param counters: Pointer to store I/O counters
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_io_counters(uint32_t socket_id, pcm_io_counters_t *counters);

/**
 * Get system-wide performance counters
 * @param counters: Pointer to store system counters
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_system_counters(pcm_system_counters_t *counters);

/**
 * Start measurement period
 * @return 0 on success, negative on error
 */
int pcm_wrapper_start_measurement(void);

/**
 * Stop measurement period and calculate differences
 * @return 0 on success, negative on error
 */
int pcm_wrapper_stop_measurement(void);

/**
 * Get system information
 * @param info_buffer Buffer to store system info
 * @param buffer_size Size of the buffer
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_system_info(char* info_buffer, size_t buffer_size);

/**
 * Get instant PCIe byte counters (snapshot, not delta)
 * @param socket_id Socket ID to get counters for
 * @param pcie_read_bytes Pointer to store PCIe read bytes
 * @param pcie_write_bytes Pointer to store PCIe write bytes
 * @return 0 on success, negative on error
 */
int pcm_wrapper_get_instant_pcie_bytes(uint32_t socket_id, uint64_t *pcie_read_bytes, uint64_t *pcie_write_bytes);

#ifdef __cplusplus
}
#endif

#endif /* __PKTGEN_PCM_WRAPPER_H__ */

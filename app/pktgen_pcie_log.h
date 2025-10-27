/*
 * PCIe Performance Logging for Pktgen
 *
 * Samples PCIe metrics at 1-second intervals with zero overhead during runtime.
 * Logs are buffered in memory and flushed to CSV on exit.
 */

#ifndef _PKTGEN_PCIE_LOG_H_
#define _PKTGEN_PCIE_LOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration */
#define PCIE_LOG_MAX_SAMPLES  3600  // 1 hour at 1 sample/sec
#define PCIE_LOG_SOCKET_ID    0     // Socket to monitor

/* Single PCIe sample */
struct pcie_sample {
    uint64_t timestamp_us;      // Microseconds since epoch
    uint64_t pci_rdcur;         // PCIRdCur counter
    uint64_t pcie_rd_bytes;     // PCIe Read bytes
    uint64_t pcie_wr_bytes;     // PCIe Write bytes
};

/**
 * Initialize PCIe logging
 * @return 0 on success, -1 on error
 */
int pcie_log_init(void);

/**
 * Start periodic sampling (called once at pktgen start)
 * @return 0 on success, -1 on error
 */
int pcie_log_start(void);

/**
 * Sample PCIe metrics (call this periodically, e.g., every stats update)
 * Automatically rate-limits to 1 sample/second
 */
void pcie_log_sample(void);

/**
 * Stop sampling and flush logs to file
 * Called automatically on pktgen exit
 */
void pcie_log_stop(void);

/**
 * Cleanup resources
 */
void pcie_log_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* _PKTGEN_PCIE_LOG_H_ */

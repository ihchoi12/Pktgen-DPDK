/*
 * PCIe Performance Logging Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <rte_timer.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#include "pktgen_pcie_log.h"
#include "common_pcm_wrapper.h"

/* Circular buffer for samples */
static struct pcie_sample samples[PCIE_LOG_MAX_SAMPLES];
static uint32_t sample_count = 0;
static uint32_t sample_index = 0;

/* Timer for periodic sampling */
static struct rte_timer sample_timer;
static int timer_initialized = 0;
static int logging_active = 0;

/* Output file path */
static const char *output_file = "pktgen-pcm-pcie.log";

/* Get current timestamp in microseconds */
static uint64_t get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* Timer callback - samples PCIe metrics */
static void sample_pcie_metrics(__rte_unused struct rte_timer *tim,
                                 __rte_unused void *arg)
{
    uint64_t pcie_rd_bytes = 0, pcie_wr_bytes = 0;

    /* Get instant PCIe counters from PCM */
    if (pcm_wrapper_get_instant_pcie_bytes(PCIE_LOG_SOCKET_ID,
                                           &pcie_rd_bytes,
                                           &pcie_wr_bytes) < 0) {
        /* Silently skip on error - don't spam logs */
        return;
    }

    /* Store sample in circular buffer */
    uint32_t idx = sample_index % PCIE_LOG_MAX_SAMPLES;
    samples[idx].timestamp_us = get_timestamp_us();
    samples[idx].pcie_rd_bytes = pcie_rd_bytes;
    samples[idx].pcie_wr_bytes = pcie_wr_bytes;

    sample_index++;
    if (sample_count < PCIE_LOG_MAX_SAMPLES) {
        sample_count++;
    }
}

/* Flush samples to CSV file */
static void flush_samples_to_file(void)
{
    FILE *fp;
    uint32_t i, start_idx;

    if (sample_count == 0) {
        printf("PCIe Log: No samples collected\n");
        return;
    }

    fp = fopen(output_file, "w");
    if (!fp) {
        fprintf(stderr, "PCIe Log: Failed to open %s for writing\n", output_file);
        return;
    }

    /* Write CSV header */
    fprintf(fp, "timestamp_us,pcie_rd_bytes,pcie_wr_bytes\n");

    /* Calculate starting index (oldest sample in circular buffer) */
    if (sample_count < PCIE_LOG_MAX_SAMPLES) {
        start_idx = 0;
    } else {
        start_idx = sample_index % PCIE_LOG_MAX_SAMPLES;
    }

    /* Write samples in chronological order */
    for (i = 0; i < sample_count; i++) {
        uint32_t idx = (start_idx + i) % PCIE_LOG_MAX_SAMPLES;
        fprintf(fp, "%lu,%lu,%lu\n",
                samples[idx].timestamp_us,
                samples[idx].pcie_rd_bytes,
                samples[idx].pcie_wr_bytes);
    }

    fclose(fp);
    printf("PCIe Log: Flushed %u samples to %s\n", sample_count, output_file);
}

int pcie_log_init(void)
{
    /* Check if PCM wrapper is available */
    if (!pcm_wrapper_is_available()) {
        printf("PCIe Log: PCM not available, logging disabled\n");
        return -1;
    }

    /* Initialize buffer */
    memset(samples, 0, sizeof(samples));
    sample_count = 0;
    sample_index = 0;

    /* Initialize timer */
    rte_timer_init(&sample_timer);
    timer_initialized = 1;

    printf("PCIe Log: Initialized (max %u samples)\n", PCIE_LOG_MAX_SAMPLES);
    return 0;
}

int pcie_log_start(void)
{
    uint64_t hz;
    unsigned lcore_id;

    if (!timer_initialized) {
        fprintf(stderr, "PCIe Log: Not initialized\n");
        return -1;
    }

    if (logging_active) {
        return 0; /* Already started */
    }

    /* Get timer frequency (TSC Hz) */
    hz = rte_get_timer_hz();

    /* Reset timer to fire every 1 second on main lcore */
    lcore_id = rte_get_main_lcore();

    if (rte_timer_reset(&sample_timer, hz, PERIODICAL, lcore_id,
                       sample_pcie_metrics, NULL) < 0) {
        fprintf(stderr, "PCIe Log: Failed to start sampling timer\n");
        return -1;
    }

    logging_active = 1;
    printf("PCIe Log: Started sampling (1 Hz on lcore %u)\n", lcore_id);
    return 0;
}

void pcie_log_stop(void)
{
    if (!logging_active) {
        return;
    }

    /* Stop timer */
    rte_timer_stop(&sample_timer);
    logging_active = 0;

    /* Flush samples to file */
    flush_samples_to_file();

    printf("PCIe Log: Stopped\n");
}

void pcie_log_cleanup(void)
{
    if (logging_active) {
        pcie_log_stop();
    }

    timer_initialized = 0;
}

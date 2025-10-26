/*
 * PCIe Performance Logging Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "pktgen_pcie_log.h"
#include "common_pcm_wrapper.h"

/* Circular buffer for samples */
static struct pcie_sample samples[PCIE_LOG_MAX_SAMPLES];
static uint32_t sample_count = 0;
static uint32_t sample_index = 0;

/* Sampling state */
static int logging_active = 0;
static uint64_t last_sample_time = 0;  // Last sample timestamp in us

/* Output file path */
static const char *output_file = "pktgen-pcm-pcie.log";

/* Get current timestamp in microseconds */
static uint64_t get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/**
 * Sample PCIe metrics
 * This should be called periodically (e.g., every stats update)
 */
void pcie_log_sample(void)
{
    uint64_t pcie_rd_bytes = 0, pcie_wr_bytes = 0;
    uint64_t now_us;

    if (!logging_active) {
        return;
    }

    /* Check if 1 second has elapsed since last sample */
    now_us = get_timestamp_us();
    if (last_sample_time != 0 && (now_us - last_sample_time) < 1000000) {
        return;  // Not yet 1 second
    }

    last_sample_time = now_us;

    /* Get instant PCIe counters from PCM */
    if (pcm_wrapper_get_instant_pcie_bytes(PCIE_LOG_SOCKET_ID,
                                           &pcie_rd_bytes,
                                           &pcie_wr_bytes) < 0) {
        /* Silently skip on error - don't spam logs */
        return;
    }

    /* Store sample in circular buffer */
    uint32_t idx = sample_index % PCIE_LOG_MAX_SAMPLES;
    samples[idx].timestamp_us = now_us;
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
    last_sample_time = 0;

    printf("PCIe Log: Initialized (max %u samples)\n", PCIE_LOG_MAX_SAMPLES);
    return 0;
}

int pcie_log_start(void)
{
    if (logging_active) {
        return 0; /* Already started */
    }

    logging_active = 1;
    last_sample_time = 0;  // Reset to trigger immediate first sample
    printf("PCIe Log: Started sampling (1 Hz)\n");
    return 0;
}

void pcie_log_stop(void)
{
    if (!logging_active) {
        return;
    }

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
}

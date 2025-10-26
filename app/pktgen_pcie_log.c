/*
 * PCIe Performance Logging Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

#include "pktgen_pcie_log.h"
#include "common_pcm_wrapper.h"

/* Circular buffer for samples */
static struct pcie_sample samples[PCIE_LOG_MAX_SAMPLES];
static uint32_t sample_count = 0;
static uint32_t sample_index = 0;

/* Sampling state */
static int logging_active = 0;

/* Thread state */
static pthread_t sampling_thread;
static volatile int thread_should_exit = 0;

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
 * Sampling thread function
 * Runs independently from main packet processing loop
 * Samples PCIe metrics at 1 Hz with ~40ms overhead per sample
 */
static void *sampling_thread_func(void *arg)
{
    (void)arg;  // Unused

    printf("PCIe Log: Sampling thread started (1 Hz, independent from main loop)\n");

    /* Wait 1 second for PCM to fully initialize before starting to sample */
    printf("PCIe Log: Waiting 1 second for PCM initialization...\n");
    sleep(1);

    if (thread_should_exit || !logging_active) {
        printf("PCIe Log: Thread exiting during initialization wait\n");
        return NULL;
    }

    /* Verify which CPU we're running on */
    int running_cpu = sched_getcpu();
    printf("PCIe Log: Starting PCIe sampling on CPU %d\n", running_cpu);

    while (!thread_should_exit && logging_active) {
        /* Get instant PCIe counters from PCM */
        uint64_t pcie_rd_bytes = 0, pcie_wr_bytes = 0;
        int ret = pcm_wrapper_get_instant_pcie_bytes(PCIE_LOG_SOCKET_ID,
                                                      &pcie_rd_bytes,
                                                      &pcie_wr_bytes);
        if (ret < 0) {
            /* Log error on first few failures */
            static int error_count = 0;
            if (error_count < 3) {
                fprintf(stderr, "PCIe Log: Failed to get PCIe bytes (error %d)\n", ret);
                error_count++;
            }
            sleep(1);  // Still sleep even on error
            continue;
        }

        /* Store sample in circular buffer */
        uint64_t now_us = get_timestamp_us();
        uint32_t idx = sample_index % PCIE_LOG_MAX_SAMPLES;
        samples[idx].timestamp_us = now_us;
        samples[idx].pcie_rd_bytes = pcie_rd_bytes;
        samples[idx].pcie_wr_bytes = pcie_wr_bytes;

        sample_index++;
        if (sample_count < PCIE_LOG_MAX_SAMPLES) {
            sample_count++;
        }

        /* No additional sleep needed - the measurement itself takes ~1 second
         * (5 groups × 200ms per group = 1000ms total) */
    }

    printf("PCIe Log: Sampling thread exiting\n");
    return NULL;
}

/**
 * Legacy sample function (no longer used - thread does sampling)
 * Kept for API compatibility but does nothing
 */
void pcie_log_sample(void)
{
    /* This function is no longer needed - sampling is done by dedicated thread */
    /* Kept for backward compatibility in case it's called from elsewhere */
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

    /* Get first sample timestamp as baseline */
    uint32_t first_idx = start_idx % PCIE_LOG_MAX_SAMPLES;
    uint64_t baseline_timestamp = samples[first_idx].timestamp_us;

    /* Write samples in chronological order with relative timestamps */
    for (i = 0; i < sample_count; i++) {
        uint32_t idx = (start_idx + i) % PCIE_LOG_MAX_SAMPLES;
        uint64_t relative_timestamp = samples[idx].timestamp_us - baseline_timestamp;
        fprintf(fp, "%lu,%lu,%lu\n",
                relative_timestamp,
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

    printf("PCIe Log: Initialized (max %u samples)\n", PCIE_LOG_MAX_SAMPLES);
    return 0;
}

int pcie_log_start(void)
{
    if (logging_active) {
        return 0; /* Already started */
    }

    logging_active = 1;
    thread_should_exit = 0;

    /* Create dedicated sampling thread */
    if (pthread_create(&sampling_thread, NULL, sampling_thread_func, NULL) != 0) {
        fprintf(stderr, "PCIe Log: Failed to create sampling thread\n");
        logging_active = 0;
        return -1;
    }

    /* Pin thread to Core 0 */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);

    int ret = pthread_setaffinity_np(sampling_thread, sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        fprintf(stderr, "PCIe Log: Warning - failed to pin thread to Core 0 (error %d)\n", ret);
    } else {
        printf("PCIe Log: Sampling thread pinned to Core 0\n");
    }

    printf("PCIe Log: Started (dedicated thread, zero overhead on main loop)\n");
    return 0;
}

void pcie_log_stop(void)
{
    if (!logging_active) {
        return;
    }

    /* Signal thread to exit */
    thread_should_exit = 1;
    logging_active = 0;

    /* Wait for thread to finish */
    pthread_join(sampling_thread, NULL);

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

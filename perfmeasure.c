#define _GNU_SOURCE
#include "perfmeasure.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <stdint.h>
#include <sched.h>

// Structure to hold event information.
struct event_info {
    const char *name;    // Event name.
    uint32_t type;       // Event type.
    uint64_t config;     // Event configuration value.
};

// Updated array of events to be measured.
struct event_info events[] = {
    {"dtc_cycles",          0x0d, 0x3 },
    {"hnf_pocq_reqs_recvd", 0x0d, 0x50005},
    {"hnf_pocq_retry",      0x0d, 0x40005},
    {"hnf_cache_miss",      0x0d, 0x10005},
    {"hnf_slc_sf_cache_access", 0x0d, 0x20005},
    {"hnf_cache_fill",      0x0d, 0x30005},
    {"hnf_mc_reqs",         0x0d, 0xd0005 },
    {"hnf_mc_retries",      0x0d, 0xc0005}
};

#define NUM_EVENTS (sizeof(events)/sizeof(events[0]))

// Array of file descriptors for the perf events.
int fds[NUM_EVENTS];

// Structure for reading perf event counters together with timing info.
struct read_format {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
};

// perf_event_open wrapper function.
static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Function to set up and enable all perf events.
void setup_and_enable_perf() {
    struct perf_event_attr pe;
    int cpu = sched_getcpu();
    if (cpu < 0) {
        perror("sched_getcpu failed");
        exit(EXIT_FAILURE);
    }
    printf("Current CPU: %d\n", cpu);

    memset(&pe, 0, sizeof(pe));
    pe.size = sizeof(struct perf_event_attr);
    pe.sample_type = PERF_SAMPLE_IDENTIFIER;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    pe.inherit = 1;
    pe.disabled = 1;         // Start events disabled.
    pe.exclude_kernel = 0;   // Include kernel mode events.
    pe.exclude_hv = 0;       // Include hypervisor events.

    for (int i = 0; i < NUM_EVENTS; i++) {
        pe.type = events[i].type;
        pe.config = events[i].config;
        fds[i] = perf_event_open(&pe, -1, cpu, -1, 0);
        if (fds[i] == -1) {
            fprintf(stderr, "Error opening event %s: %s\n", events[i].name, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    // Reset and enable all events.
    for (int i = 0; i < NUM_EVENTS; i++) {
        if (ioctl(fds[i], PERF_EVENT_IOC_RESET, 0) == -1) {
            perror("ioctl(PERF_EVENT_IOC_RESET) failed");
            exit(EXIT_FAILURE);
        }
        if (ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0) == -1) {
            perror("ioctl(PERF_EVENT_IOC_ENABLE) failed");
            exit(EXIT_FAILURE);
        }
    }
}

// Declare external global variable to store event results (defined in loadedlatency.c).
extern unsigned long long event_results[NUM_EVENTS];

// Function to disable perf events, read their counters, and store results in event_results.
void read_and_disable_perf() {
    for (int i = 0; i < NUM_EVENTS; i++) {
        if (ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0) == -1) {
            perror("ioctl(PERF_EVENT_IOC_DISABLE) failed");
            exit(EXIT_FAILURE);
        }
    }
    struct read_format rf;
    for (int i = 0; i < NUM_EVENTS; i++) {
        if (read(fds[i], &rf, sizeof(rf)) == -1) {
            perror("read failed");
            exit(EXIT_FAILURE);
        }
        event_results[i] = rf.value;
        close(fds[i]);
    }
}

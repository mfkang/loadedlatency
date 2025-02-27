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

// Array of events to be measured.
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

#define NUM_EVENTS_ARRAY (sizeof(events)/sizeof(events[0]))

// Separate file descriptor arrays for local and remote PMU monitoring.
int fds_local[NUM_EVENTS_ARRAY];
int fds_remote[NUM_EVENTS_ARRAY];

// Structure for reading perf event counters.
struct read_format {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
};

// External variables defined in loadedlatency.c.
extern unsigned long long event_results_local[NUM_EVENTS_ARRAY];
extern unsigned long long event_results_remote[NUM_EVENTS_ARRAY];

// perf_event_open wrapper function.
static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Set up and enable PMU events on the specified target node.
// For target_node==0, local monitoring on core 0 is used;
// For target_node != 0, remote monitoring is done on core 80.
void setup_and_enable_perf(int target_node) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.size = sizeof(struct perf_event_attr);
    pe.sample_type = PERF_SAMPLE_IDENTIFIER;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    pe.inherit = 1;
    pe.disabled = 1;         // Start events disabled.
    pe.exclude_kernel = 0;
    pe.exclude_hv = 0;

    // For demonstration: local monitoring on core 0, remote monitoring on core 80.
    int cpu = (target_node == 0) ? 0 : 80;
    int *fds = (target_node == 0) ? fds_local : fds_remote;

    for (int i = 0; i < NUM_EVENTS_ARRAY; i++) {
        pe.type = events[i].type;
        pe.config = events[i].config;
        fds[i] = perf_event_open(&pe, -1, cpu, -1, 0);
        if (fds[i] == -1) {
            fprintf(stderr, "Error opening event %s on target_node %d (cpu %d): %s\n",
                    events[i].name, target_node, cpu, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
    for (int i = 0; i < NUM_EVENTS_ARRAY; i++) {
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

// Disable and read PMU events on the specified target node.
// The results are stored in event_results_local for target_node==0 and in event_results_remote otherwise.
void read_and_disable_perf(int target_node) {
    int *fds = (target_node == 0) ? fds_local : fds_remote;
    unsigned long long *event_results = (target_node == 0) ? event_results_local : event_results_remote;

    for (int i = 0; i < NUM_EVENTS_ARRAY; i++) {
        if (ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0) == -1) {
            perror("ioctl(PERF_EVENT_IOC_DISABLE) failed");
            exit(EXIT_FAILURE);
        }
    }
    struct read_format rf;
    for (int i = 0; i < NUM_EVENTS_ARRAY; i++) {
        if (read(fds[i], &rf, sizeof(rf)) == -1) {
            perror("read failed");
            exit(EXIT_FAILURE);
        }
        event_results[i] = rf.value;
        close(fds[i]);
    }
}

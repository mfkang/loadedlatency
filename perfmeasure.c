#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdint.h>
#include <sched.h>

// Structure to hold event info
struct event_info {
    const char *name;    // Event name
    uint32_t type;       // Event type
    uint64_t config;     // Event id
};

struct event_info events[] = {
    {"hnf_mc_reqs",          0x0d, 0xd0005 },
    {"hnf_mc_retries",       0x0d, 0xc0005},
    {"hnf_pocq_reqs_recvd",  0x0d, 0x50005},
    {"hnf_pocq_retry",       0x0d, 0x40005}
};
#define num_events 4
int fds[num_events];

// perf_event_open wrapper
static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Structure corresponding to the read format when using
// PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING.
// It contains 3 64-bit fields: value, time_enabled, time_running.
struct read_format {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
};

// Function to set up perf events and enable them
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
    pe.disabled = 1;         // Start disabled
    pe.exclude_kernel = 0;   // Include kernel mode events (set as needed)
    pe.exclude_hv = 0;       // Include hypervisor events (set as needed)
    
    for (int i = 0; i < num_events; i++) {
        pe.type = events[i].type;
        pe.config = events[i].config;
        fds[i] = perf_event_open(&pe, -1, cpu, -1, 0);
        if (fds[i] == -1) {
            fprintf(stderr, "Error opening event %s: %s\n", events[i].name, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
    
    // Reset and enable all events
    for (int i = 0; i < num_events; i++) {
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

// Disable perf events, read and print counter data, and close file descriptors
void read_and_disable_perf() {
    // Disable all events
    for (int i = 0; i < num_events; i++) {
        if (ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0) == -1) {
            perror("ioctl(PERF_EVENT_IOC_DISABLE) failed");
            exit(EXIT_FAILURE);
        }
    }
    
    // Read and print the 24-byte counter data for each event.
    struct read_format rf;
    for (int i = 0; i < num_events; i++) {
        if (read(fds[i], &rf, sizeof(rf)) == -1) {
            perror("read failed");
            exit(EXIT_FAILURE);
        }
        printf("Event %s:\n", events[i].name);
        printf("  value:         %llu\n", (unsigned long long)rf.value);
//        printf("  time_enabled:  %llu\n", (unsigned long long)rf.time_enabled);
//        printf("  time_running:  %llu\n", (unsigned long long)rf.time_running);
        close(fds[i]);
    }
}

#ifdef _standalone_
// The region of interest (workload) to be measured
void workload_region(void) {
    // Replace this dummy workload with your actual code region.
    volatile int dummy = 0;
    for (int i = 0; i < 100000000; i++) {
        dummy += i;
    }
}

int main(void)
{
    // Setup and enable perf events
    setup_and_enable_perf();
    
    // Execute the workload region to be measured
    workload_region();
    
    // Read the perf data, disable events, and close file descriptors
    read_and_disable_perf();
    
    return 0;
}
#endif

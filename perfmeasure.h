#ifndef PERFMEASURE_H
#define PERFMEASURE_H

#include <stdint.h>

// Structure to hold event information.
struct event_info {
    const char *name;    // Event name.
    uint32_t type;       // Event type.
    uint64_t config;     // Event configuration value.
};

// Declaration of the events array.
extern struct event_info events[];

// Declarations for functions that set up and read performance events with a target node parameter.
// target_node: 0 for local monitoring (e.g., core 0), nonzero for remote monitoring (e.g., core 80).
void setup_and_enable_perf(int target_node);
void read_and_disable_perf(int target_node);

#endif // PERFMEASURE_H

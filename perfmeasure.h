#ifndef PERFMEASURE_H
#define PERFMEASURE_H

// Declarations for functions that set up and read performance events.
// These functions are implemented in perfmeasure.c.
void setup_and_enable_perf();
void read_and_disable_perf();

#endif // PERFMEASURE_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <numa.h>
#include <numaif.h>
#include "perfmeasure.h"

#define ARRAY_SIZE (512 * 1024 * 1024) // 512MB for DDR access.
#define STREAM_TYPE double
#define MAX_THREADS 128              // Maximum number of threads.
#define NUM_EVENTS 8

// Global variables for memory allocation and measurement.
volatile uintptr_t *ptr_array;
STREAM_TYPE *B, *C;
int num_threads;      // Number of threads used in the current measurement.
int start_core;       // Starting core for thread pinning.
int stride;           // Memory access stride.
int mem_node;         // Memory node for allocation.
volatile int latency_done = 0;
double total_bandwidth = 0.0;
pthread_mutex_t bandwidth_mutex;
pthread_barrier_t sync_barrier;

// Global measurement results.
double measured_latency = 0.0;
double measured_total_bandwidth = 0.0;
double measured_thread_bw[MAX_THREADS] = {0};

// Global arrays to store PMU event counts.
unsigned long long event_results_local[NUM_EVENTS] = {0};
unsigned long long event_results_remote[NUM_EVENTS] = {0};

// Bind the current thread to the specified CPU core.
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// Randomize the pointer chasing chain.
void randomize_map(uintptr_t *arr, int len, int stride) {
    int num_elements = len / stride;
    if (num_elements <= 1) {
        fprintf(stderr, "Invalid stride: not enough elements!\n");
        exit(EXIT_FAILURE);
    }
    int *indices = malloc(num_elements * sizeof(int));
    if (!indices) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < num_elements; i++) {
        indices[i] = i * stride;
    }
    srand(time(NULL));
    for (int i = num_elements - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    for (int i = 0; i < num_elements - 1; i++) {
        arr[indices[i]] = (uintptr_t)&arr[indices[i + 1]];
    }
    arr[indices[num_elements - 1]] = (uintptr_t)&arr[indices[0]];
    free(indices);
}

// Allocate and initialize memory on the specified NUMA node.
void setup_memory(void) {
    int use_numa = (numa_available() != -1);
    if (use_numa) {
        ptr_array = (volatile uintptr_t *)numa_alloc_onnode(ARRAY_SIZE, mem_node);
        B = (STREAM_TYPE *)numa_alloc_onnode(ARRAY_SIZE, mem_node);
        C = (STREAM_TYPE *)numa_alloc_onnode(ARRAY_SIZE, mem_node);
    } else {
        ptr_array = (volatile uintptr_t *)malloc(ARRAY_SIZE);
        B = (STREAM_TYPE *)malloc(ARRAY_SIZE);
        C = (STREAM_TYPE *)malloc(ARRAY_SIZE);
    }
    if (!ptr_array || !B || !C) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    memset((void *)ptr_array, 0, ARRAY_SIZE);
    memset((void *)B, 0, ARRAY_SIZE);
    memset((void *)C, 0, ARRAY_SIZE);
    size_t len_ptr = ARRAY_SIZE / sizeof(uintptr_t);
    for (size_t i = 0; i < len_ptr; i++) {
        ptr_array[i] = (uintptr_t)&ptr_array[i];
    }
    size_t len_stream = ARRAY_SIZE / sizeof(STREAM_TYPE);
    for (size_t i = 0; i < len_stream; i++) {
        B[i] = 2.0;
        C[i] = 0.0;
    }
    randomize_map((uintptr_t *)ptr_array, len_ptr, stride);
}

// Local latency measurement thread function (runs on core 0).
// It sets up local PMU events, waits on the barrier, performs pointer chasing,
// then reads local PMU counters.
void *measure_latency(void *arg) {
    pin_thread_to_core(0);
    int len = ARRAY_SIZE / sizeof(STREAM_TYPE);
    setup_and_enable_perf(0);  // Setup local PMU (target node 0)
    pthread_barrier_wait(&sync_barrier);

    struct timeval start, end;
    gettimeofday(&start, NULL);
    volatile uintptr_t *p = &ptr_array[0];
    // Perform pointer chasing 10 times.
    for (int rep = 0; rep < 10; rep++) {
        for (int i = 0; i < len; i += stride) {
            p = (volatile uintptr_t *) *p;
        }
    }
    latency_done = 1;  // Signal remote PMU and bandwidth threads to stop.
    gettimeofday(&end, NULL);
    read_and_disable_perf(0);  // Read local PMU counters.
    
    double elapsed = (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec);
    measured_latency = (elapsed / ((len / stride) * 10)) * 1000;
    return NULL;
}

// Remote PMU monitoring thread function (runs on remote CPU, e.g., core 80).
// This thread only monitors remote PMU events without pointer chasing.
void *remote_pmu_monitor(void *arg) {
    pin_thread_to_core(80);
    setup_and_enable_perf(1);  // Setup remote PMU (target node 1)
    pthread_barrier_wait(&sync_barrier);
    while (!latency_done) usleep(1);
    read_and_disable_perf(1);  // Read remote PMU counters.
    return NULL;
}

// DDR stream bandwidth measurement thread function.
void *stream_bandwidth(void *arg) {
    int thread_id = *(int *)arg;
    pin_thread_to_core(start_core + thread_id);
    size_t len = ARRAY_SIZE / (sizeof(STREAM_TYPE) * num_threads);
    STREAM_TYPE *local_B = B + thread_id * len;
    STREAM_TYPE *local_C = C + thread_id * len;
    pthread_barrier_wait(&sync_barrier);
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    int j;
    size_t i = 0;
    for (j = 0; !latency_done; j++) {
        size_t i_end = len / 8 * 8;
        for (i = 0; i < i_end; i += 8) {
#if 0
            local_C[i] = local_B[i];
            local_C[i+1] = local_B[i+1];
            local_C[i+2] = local_B[i+2];
            local_C[i+3] = local_B[i+3];
            local_C[i+4] = local_B[i+4];
            local_C[i+5] = local_B[i+5];
            local_C[i+6] = local_B[i+6];
            local_C[i+7] = local_B[i+7];
#else
            double *src_ptr = &local_B[i];
            double *dst_ptr = &local_C[i];
            asm volatile(
                "ldnp d0, d1, [%[src], #16] \n\t"  // Load two doubles from src+16
                "stnp d0, d1, [%[dst], #16] \n\t"  // Store them to dst+16
                "ldnp d2, d3, [%[src], #32] \n\t"  // Load two doubles from src+32
                "stnp d2, d3, [%[dst], #32] \n\t"  // Store them to dst+32
                "ldnp d4, d5, [%[src], #48] \n\t"  // Load two doubles from src+48
                "stnp d4, d5, [%[dst], #48] \n\t"  // Store them to dst+48
                "ldnp d6, d7, [%[src], #64] \n\t"  // Load two doubles from src+64
                "stnp d6, d7, [%[dst], #64] \n\t"  // Store them to dst+64
                "add %[src], %[src], #64 \n\t"     // Advance src by 64 bytes
                "add %[dst], %[dst], #64 \n\t"     // Advance dst by 64 bytes
                : [src] "+r" (src_ptr), [dst] "+r" (dst_ptr)
                :
                : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory"
            );
#endif
            if (latency_done) break;
        }
        for (; i < len; i++) {
            local_C[i] = local_B[i];
            if (latency_done) break;
        }
    }
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec);
    double bandwidth = (2 * ((j - 1) * len + i) * sizeof(STREAM_TYPE)) / (elapsed * 1e-6) / (1024.0 * 1024.0 * 1024.0);
    pthread_mutex_lock(&bandwidth_mutex);
    total_bandwidth += bandwidth;
    pthread_mutex_unlock(&bandwidth_mutex);
    measured_thread_bw[thread_id] = bandwidth;
    return NULL;
}

// run_measurement: Launches bandwidth threads, local latency thread, and (if mem_node != 0) remote PMU monitoring thread.
// The barrier count is: number of bandwidth threads + 1 (latency thread) + (remote thread ? 1 : 0).
void run_measurement(void) {
    pthread_t latency_thread, remote_thread;
    int remote_needed = (mem_node != 0);
    int barrier_count = num_threads + 1 + (remote_needed ? 1 : 0);
    pthread_barrier_init(&sync_barrier, NULL, barrier_count);

    pthread_t *bandwidth_threads = malloc(num_threads * sizeof(pthread_t));
    int *thread_ids = malloc(num_threads * sizeof(int));
    if (!bandwidth_threads || !thread_ids) {
        perror("Allocation failed");
        exit(EXIT_FAILURE);
    }
    
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i;
        pthread_create(&bandwidth_threads[i], NULL, stream_bandwidth, &thread_ids[i]);
    }
    
    pthread_create(&latency_thread, NULL, measure_latency, NULL);
    if (remote_needed) {
        pthread_create(&remote_thread, NULL, remote_pmu_monitor, NULL);
    }
    
    pthread_join(latency_thread, NULL);
    for (int i = 0; i < num_threads; i++) {
        pthread_join(bandwidth_threads[i], NULL);
    }
    if (remote_needed) {
        pthread_join(remote_thread, NULL);
    }
    measured_total_bandwidth = total_bandwidth;
    pthread_barrier_destroy(&sync_barrier);
    free(bandwidth_threads);
    free(thread_ids);
}

// Clean up allocated memory.
void cleanup_memory(void) {
    if (numa_available() != -1) {
        numa_free((void *)ptr_array, ARRAY_SIZE);
        numa_free(B, ARRAY_SIZE);
        numa_free(C, ARRAY_SIZE);
    } else {
        free((void *)ptr_array);
        free(B);
        free(C);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <min_num_threads> <max_num_threads> <start_core> <stride> <mem_node>\n", argv[0]);
        return -1;
    }
    int min_threads = atoi(argv[1]);
    int max_threads = atoi(argv[2]);
    start_core = atoi(argv[3]);
    stride = atoi(argv[4]);
    mem_node = atoi(argv[5]);
    num_threads = max_threads;  // Use max_threads for memory allocation.
    
    pthread_mutex_init(&bandwidth_mutex, NULL);
    setup_memory();
    
    FILE *fp = fopen("results.txt", "w");
    if (!fp) {
        perror("Failed to open results.txt");
        exit(EXIT_FAILURE);
    }
    
    // Write header: includes latency, total bandwidth, local and remote PMU events, and per-thread bandwidth.
    fprintf(fp, "Num_Threads\tDDR Read Latency ( ns)\tTotal DDR Stream Bandwidth (GB/s)\t");
    for (int i = 0; i < NUM_EVENTS; i++) {
        fprintf(fp, "Local_%s\t", events[i].name);
    }
    if (mem_node != 0) {
        for (int i = 0; i < NUM_EVENTS; i++) {
            fprintf(fp, "Remote_%s\t", events[i].name);
        }
    }
    for (int i = 0; i < max_threads; i++) {
        fprintf(fp, "Thread %d Bandwidth\t", i);
    }
    fprintf(fp, "\n");
    
    // Loop through thread counts from min_threads to max_threads.
    for (int cur = min_threads; cur <= max_threads; cur++) {
        total_bandwidth = 0.0;
        latency_done = 0;
        measured_latency = 0.0;
        measured_total_bandwidth = 0.0;
        for (int i = 0; i < MAX_THREADS; i++) {
            measured_thread_bw[i] = 0.0;
        }
        for (int i = 0; i < NUM_EVENTS; i++) {
            event_results_local[i] = 0;
            event_results_remote[i] = 0;
        }
        
        num_threads = cur;
        run_measurement();
        
        fprintf(fp, "%d\t%.2f\t%.2f\t", cur, measured_latency, measured_total_bandwidth);
        for (int i = 0; i < NUM_EVENTS; i++) {
            fprintf(fp, "%llu\t", event_results_local[i]);
        }

        if (mem_node != 0) {
            for (int i = 0; i < NUM_EVENTS; i++) {
                fprintf(fp, "%llu\t", event_results_remote[i]);
            }
        }
        for (int i = 0; i < max_threads; i++) {
            if (i < cur)
                fprintf(fp, "%.2f\t", measured_thread_bw[i]);
            else
                fprintf(fp, "NA\t");
        }
        fprintf(fp, "\n");
    }
    
    fclose(fp);
    cleanup_memory();
    pthread_mutex_destroy(&bandwidth_mutex);
    return 0;
}

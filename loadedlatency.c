#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <numa.h>          // Use libnuma for NUMA support.
#include <numaif.h>
#include "perfmeasure.h"   // Include the header for perf measurement functions.

#define ARRAY_SIZE (512 * 1024 * 1024) // 512MB to ensure DDR access.
#define STREAM_TYPE double
#define MAX_THREADS 128  // Maximum number of threads supported.

// Global variables for memory allocation and measurement.
volatile uintptr_t *ptr_array;
STREAM_TYPE *B, *C;
int num_threads;      // Number of threads used in the current measurement.
int start_core;       // Starting core for thread pinning.
int stride;           // Memory access stride.
int mem_node;         // Memory node for allocation.
volatile int latency_done = 0;  // Flag to signal that latency measurement is finished.

double total_bandwidth = 0.0;   // Accumulated bandwidth from all threads.
pthread_mutex_t bandwidth_mutex; // Mutex to protect bandwidth accumulation.
pthread_barrier_t sync_barrier;    // Barrier to synchronize threads.

// Global variables to store measurement results.
double measured_latency = 0.0;          // Memory read latency (in ns).
double measured_total_bandwidth = 0.0;    // Total DDR stream bandwidth (in GB/s).
double measured_thread_bw[MAX_THREADS] = {0}; // Bandwidth per thread (in GB/s).
// Perf event counter results (order: dtc_cycles, hnf_pocq_reqs_recvd, hnf_pocq_retry,
// hnf_cache_miss, hnf_slc_sf_cache_access, hnf_cache_fill, hnf_mc_reqs, hnf_mc_retries).
unsigned long long event_results[8] = {0};

// Function to bind the current thread to a specified CPU core.
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t current_thread = pthread_self();
    pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

// Function to randomize the pointer chasing chain in memory.
void randomize_map(uintptr_t *arr, int len, int stride) {
    int i, j, temp;
    int num_elements = len / stride;
    if (num_elements <= 1) {
        fprintf(stderr, "Invalid stride: not enough elements!\n");
        exit(EXIT_FAILURE);
    }
    int *indices = malloc(num_elements * sizeof(int));
    if (!indices) {
        perror("malloc failed for indices");
        exit(EXIT_FAILURE);
    }
    
    for (i = 0; i < num_elements; i++) {
        indices[i] = i * stride;
    }
    
    srand(time(NULL));
    for (i = num_elements - 1; i > 0; i--) {
        j = rand() % (i + 1);
        temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
    
    for (i = 0; i < num_elements - 1; i++) {
        arr[indices[i]] = (uintptr_t)&arr[indices[i + 1]];
    }
    arr[indices[num_elements - 1]] = (uintptr_t)&arr[indices[0]]; // Create a loop.
    free(indices);
}

// Function to set up memory on the specified NUMA node.
// The allocation is based on the maximum number of threads.
void setup_memory(void) {
    int use_numa = 0;
    if (numa_available() != -1) {
        use_numa = 1;
    }

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
        ptr_array[i] = (uintptr_t)&ptr_array[i]; // Initialize self-loop.
    }
    size_t len_stream = ARRAY_SIZE / sizeof(STREAM_TYPE);
    for (size_t i = 0; i < len_stream; i++) {
        B[i] = 2.0;
        C[i] = 0.0;
    }
    // Randomize the pointer chain for memory access.
    randomize_map((uintptr_t *)ptr_array, len_ptr, stride);
}

// Thread function to measure memory read latency.
void *measure_latency(void *arg) {
    // Bind this thread to core 0.
    pin_thread_to_core(0);
    struct timeval start, end;
    int len = ARRAY_SIZE / sizeof(STREAM_TYPE);
    
    // Set up and enable perf events.
    setup_and_enable_perf();
    
    // Wait until all measurement threads (latency and bandwidth) are ready.
    pthread_barrier_wait(&sync_barrier);

    volatile uintptr_t *p = &ptr_array[0];
    gettimeofday(&start, NULL);
    // Perform pointer chasing 10 times.
    for (int rep = 0; rep < 10; rep++) {
        for (int i = 0; i < len; i += stride) {
            p = (volatile uintptr_t *) *p;
        }
    }
    latency_done = 1;  // Signal bandwidth threads to stop.
    gettimeofday(&end, NULL);
    // Read and disable perf events; the counter values are stored in event_results.
    read_and_disable_perf();
    
    double elapsed = (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec);
    // Calculate average latency in nanoseconds.
    measured_latency = (elapsed / ((len / stride) * 10)) * 1000;
    return NULL;
}

// Thread function to measure DDR stream bandwidth.
void *stream_bandwidth(void *arg) {
    int thread_id = *(int *)arg;
    // Bind the thread to a specific core: start_core + thread_id.
    pin_thread_to_core(start_core + thread_id);
    
    struct timeval start, end;
    size_t len = ARRAY_SIZE / (sizeof(STREAM_TYPE) * num_threads);
    STREAM_TYPE *local_B = B + thread_id * len;
    STREAM_TYPE *local_C = C + thread_id * len;
    
    // Wait until all threads are ready.
    pthread_barrier_wait(&sync_barrier);
    
    int j;
    size_t i = 0;
    gettimeofday(&start, NULL);
    // Copy data repeatedly until latency measurement is done.
    for (j = 0; !latency_done; ++j) {
        size_t i_end = len / 8 * 8;
        for (i = 0; i < i_end; i += 8) {
            local_C[i] = local_B[i];
            local_C[i + 1] = local_B[i + 1];
            local_C[i + 2] = local_B[i + 2];
            local_C[i + 3] = local_B[i + 3];
            local_C[i + 4] = local_B[i + 4];
            local_C[i + 5] = local_B[i + 5];
            local_C[i + 6] = local_B[i + 6];
            local_C[i + 7] = local_B[i + 7];
            if (latency_done) break;
        }
        for (; i < len; i++) {
            local_C[i] = local_B[i];
            if (latency_done) break;
        }
    }
    gettimeofday(&end, NULL);
    
    double elapsed = (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec);
    // Calculate bandwidth in GB/s.
    double bandwidth = (2 * ((j - 1) * len + i) * sizeof(STREAM_TYPE)) / (elapsed * 1e-6) / (1024.0 * 1024.0 * 1024.0);
    
    // Safely add to the total bandwidth.
    pthread_mutex_lock(&bandwidth_mutex);
    total_bandwidth += bandwidth;
    pthread_mutex_unlock(&bandwidth_mutex);
    
    // Save individual thread bandwidth.
    measured_thread_bw[thread_id] = bandwidth;
    return NULL;
}

// Function to run one measurement iteration using the current number of threads.
void run_measurement(void) {
    pthread_t latency_thread;
    pthread_t *bandwidth_threads = malloc(num_threads * sizeof(pthread_t));
    int *thread_ids = malloc(num_threads * sizeof(int));
    if (!bandwidth_threads || !thread_ids) {
        perror("Allocation failed for thread arrays");
        exit(EXIT_FAILURE);
    }
    
    // Initialize the barrier for (num_threads + 1) threads (bandwidth threads plus the latency thread).
    pthread_barrier_init(&sync_barrier, NULL, num_threads + 1);
    
    // Create bandwidth measurement threads.
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i;
        pthread_create(&bandwidth_threads[i], NULL, stream_bandwidth, &thread_ids[i]);
    }
    
    // Create the latency measurement thread.
    pthread_create(&latency_thread, NULL, measure_latency, NULL);
    
    // Wait for the latency thread to finish.
    pthread_join(latency_thread, NULL);
    // Wait for all bandwidth threads to finish.
    for (int i = 0; i < num_threads; i++) {
        pthread_join(bandwidth_threads[i], NULL);
    }
    measured_total_bandwidth = total_bandwidth;
    
    pthread_barrier_destroy(&sync_barrier);
    free(bandwidth_threads);
    free(thread_ids);
}

// Function to clean up allocated memory.
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
    // New usage: <min_num_threads> <max_num_threads> <start_core> <stride> <mem_node>
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <min_num_threads> <max_num_threads> <start_core> <stride> <mem_node>\n", argv[0]);
        return -1;
    }
    int min_threads = atoi(argv[1]);
    int max_threads = atoi(argv[2]);
    start_core = atoi(argv[3]);
    stride = atoi(argv[4]);
    mem_node = atoi(argv[5]);
    
    // For memory allocation, set num_threads to max_threads.
    num_threads = max_threads;
    
    // Initialize the mutex.
    pthread_mutex_init(&bandwidth_mutex, NULL);
    
    // Allocate and initialize memory on the specified NUMA node.
    setup_memory();
    
    // Open the results file to write the measurement output.
    FILE *fp = fopen("results.txt", "w");
    if (!fp) {
        perror("Failed to open results.txt");
        exit(EXIT_FAILURE);
    }
    
    // Write header line to the results file.
    // The header lists: Num_Threads, DDR Read Latency (ns), Total DDR Stream Bandwidth (GB/s),
    // followed by the eight performance events (in the new order),
    // and then one column per thread for thread-specific bandwidth.
    fprintf(fp, "Num_Threads\tDDR Read Latency ( ns)\tTotal DDR Stream Bandwidth (GB/s)\t"
                "dtc_cycles\thnf_pocq_reqs_recvd\thnf_pocq_retry\thnf_cache_miss\t"
                "hnf_slc_sf_cache_access\thnf_cache_fill\thnf_mc_reqs\thnf_mc_retries");
    for (int i = 0; i < max_threads; i++) {
        fprintf(fp, "\tThread %d Bandwidth", i);
    }
    fprintf(fp, "\n");
    
    // Loop through thread counts from min_threads to max_threads.
    for (int cur = min_threads; cur <= max_threads; cur++) {
        // Reset global measurement variables.
        total_bandwidth = 0.0;
        latency_done = 0;
        measured_latency = 0.0;
        measured_total_bandwidth = 0.0;
        for (int i = 0; i < MAX_THREADS; i++) {
            measured_thread_bw[i] = 0.0;
        }
        for (int i = 0; i < 8; i++) {
            event_results[i] = 0;
        }
        
        // Set the current number of threads for this measurement.
        num_threads = cur;
        // Run one measurement.
        run_measurement();
        
        // Output the measurement results to the file.
        fprintf(fp, "%d\t%.2f\t%.2f\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu",
                cur,
                measured_latency,
                measured_total_bandwidth,
                event_results[0],
                event_results[1],
                event_results[2],
                event_results[3],
                event_results[4],
                event_results[5],
                event_results[6],
                event_results[7]);
        // Output the bandwidth for each thread; if the thread index is not used, print "NA".
        for (int i = 0; i < max_threads; i++) {
            if (i < cur)
                fprintf(fp, "\t%.2f", measured_thread_bw[i]);
            else
                fprintf(fp, "\tNA");
        }
        fprintf(fp, "\n");
    }
    
    fclose(fp);
    cleanup_memory();
    pthread_mutex_destroy(&bandwidth_mutex);
    return 0;
}

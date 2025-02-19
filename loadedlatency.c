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
#include <numa.h>          // libnuma
#include <numaif.h>        // for numa_bind

#define ARRAY_SIZE (512 * 1024 * 1024) // 512MB to ensure DDR access
#define STREAM_TYPE double

extern void setup_and_enable_perf();
extern void read_and_disable_perf();

// Global variables
volatile uintptr_t *ptr_array;
STREAM_TYPE *B, *C;
int num_threads;
int start_core;
int stride;
int mem_node;              // memory node input parameter
volatile int latency_done = 0;

double total_bandwidth = 0.0;  // Total bandwidth across all threads
pthread_mutex_t bandwidth_mutex;  // Mutex for thread-safe bandwidth accumulation
pthread_barrier_t sync_barrier;     // Barrier to synchronize gettimeofday calls

// Bind thread to specific core
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t current_thread = pthread_self();
    pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

// Randomize memory access pattern using pointer chasing
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
    arr[indices[num_elements - 1]] = (uintptr_t)&arr[indices[0]]; // Loop back
    
    free(indices);
}

// Setup memory: allocate ptr_array, B, C on specified NUMA node, initialize arrays and randomize chain.
void setup_memory(void) {
    int use_numa = 0;
    if (numa_available() != -1) {
        use_numa = 1;
    }

    if (use_numa) {
        // Allocate memory on specified node
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

    // Initialize arrays
    size_t len_ptr = ARRAY_SIZE / sizeof(uintptr_t);
    for (size_t i = 0; i < len_ptr; i++) {
        ptr_array[i] = (uintptr_t)&ptr_array[i];  // initial self-loop
    }
    size_t len_stream = ARRAY_SIZE / sizeof(STREAM_TYPE);
    for (size_t i = 0; i < len_stream; i++) {
        B[i] = 2.0;
        C[i] = 0.0;
    }

    // Randomize the pointer chasing chain
    randomize_map((uintptr_t *)ptr_array, len_ptr, stride);
}

// Workload functions in separate threads need to synchronize before starting measurement

// Measure memory read latency
void *measure_latency(void *arg) {
    // Bind latency measurement thread to core 0
    pin_thread_to_core(0);
    struct timeval start, end;
    int len = ARRAY_SIZE / sizeof(STREAM_TYPE);
   
    setup_and_enable_perf();
    // Wait at barrier until all measurement threads are ready
    pthread_barrier_wait(&sync_barrier);

    volatile uintptr_t *p = &ptr_array[0];
    gettimeofday(&start, NULL);

    for (int rep = 0; rep < 10; rep++) {
        for (int i = 0; i < len; i += stride) {
            p = (volatile uintptr_t *) *p;
        }
    }

    latency_done = 1;  // Signal bandwidth threads to finish
    gettimeofday(&end, NULL);
    read_and_disable_perf();  
   
    double elapsed = (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec);
    double latency = elapsed / (len / stride) / 10; // rep = 10
    printf("Memory Read Latency: %.2f ns\n", latency * 1000);
//    printf("Latency thread started at %ld.%06ld, ended at %ld.%06ld\n",
//           start.tv_sec, start.tv_usec, end.tv_sec, end.tv_usec);
    return NULL;
}

// Measure memory bandwidth per thread
void *stream_bandwidth(void *arg) {
    int thread_id = *(int *)arg;
    // Bind thread to core: start_core + thread_id
    pin_thread_to_core(start_core + thread_id);
    
    struct timeval start, end;
    size_t len = ARRAY_SIZE / (sizeof(STREAM_TYPE) * num_threads);
    STREAM_TYPE *local_B = B + thread_id * len;
    STREAM_TYPE *local_C = C + thread_id * len;
    
    // Wait at barrier until all measurement threads are ready
    pthread_barrier_wait(&sync_barrier);

    // Perform memory bandwidth workload until latency measurement finishes
    int j;
    size_t i = 0;
    gettimeofday(&start, NULL);
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
    double bandwidth = (2 * ((j - 1) * len + i) * sizeof(STREAM_TYPE)) / (elapsed * 1e-6) / (1024 * 1024 * 1024);

    pthread_mutex_lock(&bandwidth_mutex);
    total_bandwidth += bandwidth;
    pthread_mutex_unlock(&bandwidth_mutex);

    printf("Thread %d - DDR Stream Bandwidth: %.2f GB/s\n", thread_id, bandwidth);
//    printf("Bandwidth thread %d started at %ld.%06ld, ended at %ld.%06ld\n",
//           thread_id, start.tv_sec, start.tv_usec, end.tv_sec, end.tv_usec);
//    printf("Thread %d bandwidth measurement i = %ld, j = %d, len = %ld \n", thread_id, i, j, len );

    return NULL;
}

// Run measurement: create threads, wait for them to finish
void run_measurement(void) {
    pthread_t latency_thread;
    pthread_t *bandwidth_threads = malloc(num_threads * sizeof(pthread_t));
    int *thread_ids = malloc(num_threads * sizeof(int));
    if (!bandwidth_threads || !thread_ids) {
        perror("Allocation failed for thread arrays");
        exit(EXIT_FAILURE);
    }

    // Initialize barrier for (num_threads + 1) threads (bandwidth threads + latency thread)
    pthread_barrier_init(&sync_barrier, NULL, num_threads + 1);
    
    // Create bandwidth measurement threads
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i;
        pthread_create(&bandwidth_threads[i], NULL, stream_bandwidth, &thread_ids[i]);
    }
    
    // Create latency measurement thread
    pthread_create(&latency_thread, NULL, measure_latency, NULL);
    
    // Wait for threads to finish
    pthread_join(latency_thread, NULL);
    for (int i = 0; i < num_threads; i++) {
        pthread_join(bandwidth_threads[i], NULL);
    }

    printf("Total DDR Stream Bandwidth: %.2f GB/s\n", total_bandwidth);

    pthread_barrier_destroy(&sync_barrier);
    free(bandwidth_threads);
    free(thread_ids);
}

// Cleanup memory: free allocated memory (using numa_free if allocated by libnuma)
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
    // New usage: <program> <num_threads> <start_core> <stride> <mem_node>
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <num_threads> <start_core> <stride> <mem_node>\n", argv[0]);
        return -1;
    }

    num_threads = atoi(argv[1]);
    start_core = atoi(argv[2]);
    stride = atoi(argv[3]);
    mem_node = atoi(argv[4]);

    // Initialize mutex
    pthread_mutex_init(&bandwidth_mutex, NULL);

    // Setup memory on specified NUMA node and initialize arrays
    setup_memory();

    // Run measurement threads (latency and bandwidth)
    run_measurement();

    // Cleanup memory and mutex
    cleanup_memory();
    pthread_mutex_destroy(&bandwidth_mutex);

    return 0;
}

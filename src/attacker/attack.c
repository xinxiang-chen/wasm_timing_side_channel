#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

// --- TIMER SETUP ---
static _Atomic long counter = 0;
static _Atomic int keep_running = 1;

static const int mix[12] = {1, 7, 2, 8, 3, 9, 4, 10, 5, 11, 6, 12};

// FIX 1: Replaced x86 'lfence' with a Wasm-compatible C11 memory barrier
static inline void serialize_cpu() {
    atomic_thread_fence(memory_order_seq_cst);
}

void* timer_thread(void* arg) {
    // FIX 2: Removed core pinning. We must rely on the JS Engine / OS scheduler 
    // to put this Web Worker on a separate physical core.
    while (atomic_load_explicit(&keep_running, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
    }
    return NULL;
}

// --- MEMORY ALLOCATIONS ---
__attribute__((aligned(4096))) volatile uint8_t table[0x6000]; 
#define OBSERVER_BUF_SIZE (12 * 0x1000 + 0x1000) 
static uint8_t *observer_buf;

// --- VICTIM MODULE ---
__attribute__((noinline)) 
uint8_t leaky_access(uint8_t secret_bit) {
    uint32_t offsets[3] = {0x1800, 0x1040, 0x1400}; // map: set32, 1, 16
    return table[offsets[secret_bit]]; 
}

// --- OBSERVER MODULE ---
__attribute__((noinline))
void prime_set_0() {
    volatile uint8_t sink = 0;
    for (int i = 0; i < 12; i++) { sink ^= observer_buf[(mix[i] * 0x1000) + 0x40]; }
}

__attribute__((noinline))
void probe_set_0() {
    volatile uint8_t sink = 0;
    for (int i = 0; i < 12; i++) { sink ^= observer_buf[(mix[i] * 0x1000) + 0x40]; }
}

// --- DATA COLLECTION LOGIC ---
long measure_micro_batch(uint8_t secret_bit, int batch_size) {
    long total_ticks = 0;
    
    for(int i = 0; i < batch_size; i++) {
        prime_set_0();
        serialize_cpu();

        leaky_access(secret_bit);
        serialize_cpu();

        long start = atomic_load_explicit(&counter, memory_order_acquire);
        serialize_cpu();

        probe_set_0();
        serialize_cpu();

        long end = atomic_load_explicit(&counter, memory_order_acquire);
        serialize_cpu();
        
        total_ticks += (end - start);
    }
    
    return total_ticks;
}

int main(void) {
    pthread_t thread_id;

    // FIX 3: Removed main thread core pinning.
    observer_buf = aligned_alloc(4096, OBSERVER_BUF_SIZE);
    if (!observer_buf) return 1;
    for (size_t i = 0; i < OBSERVER_BUF_SIZE; i++) observer_buf[i] = (uint8_t)(i & 0xff);
    for (size_t i = 0; i < 0x6000; i++) table[i] = (uint8_t)(i & 0xff);

    if (pthread_create(&thread_id, NULL, timer_thread, NULL) != 0) return 1;
    while (atomic_load_explicit(&counter, memory_order_relaxed) == 0) {}

    int num_samples = 10000;  
    int batch_size = 100;     
    
    long *results_base = malloc(num_samples * sizeof(long));
    long *results_0 = malloc(num_samples * sizeof(long));
    long *results_1 = malloc(num_samples * sizeof(long));

    for(int i = 0; i < 1000; i++) {
        measure_micro_batch(2, batch_size);
        measure_micro_batch(0, batch_size);
        measure_micro_batch(1, batch_size);
    }

    srand(time(NULL));

    // THE RANDOMIZED EXPERIMENT
    for (int i = 0; i < num_samples; i++) {
        int order[3] = {0, 1, 2};

        for (int j = 2; j > 0; j--) {
            int k = rand() % (j + 1); 
            int temp = order[j];
            order[j] = order[k];
            order[k] = temp;
        }

        for (int j = 0; j < 3; j++) {
            int current_secret = order[j];
            long ticks = measure_micro_batch(current_secret, batch_size);

            if (current_secret == 0) {
                results_0[i] = ticks;
            } else if (current_secret == 1) {
                results_1[i] = ticks;
            } else {
                results_base[i] = ticks;
            }
        }
    }

    printf("base,secret_0,secret_1\n");
    for (int i = 0; i < num_samples; i++) {
        printf("%ld,%ld,%ld\n", results_base[i], results_0[i], results_1[i]);
    }

    atomic_store_explicit(&keep_running, 0, memory_order_relaxed);
    pthread_join(thread_id, NULL);
    free(observer_buf);
    free(results_0);
    free(results_1);
    free(results_base);
    
    return 0;
}
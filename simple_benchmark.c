/*
 * simple_benchmark.c
 *
 * Minimal test program with exactly one false sharing instance.
 * Two threads write to adjacent fields on the same cache line.
 *
 * Use this during early PIN tool development so you know exactly
 * what your tool should find before adding complexity.
 *
 * Compile:
 *   gcc -O0 -g -pthread -o simple_benchmark simple_benchmark.c
 *
 * Run natively:
 *   ./simple_benchmark
 *
 * Run under PIN:
 *   pin -t /path/to/false_sharing_detector.so -- ./simple_benchmark
 *
 * Expected PIN tool output:
 *   Exactly ONE hotspot on the cache line containing the Counters struct.
 *   Two writing threads: thread 0 (main) and thread 1 (worker).
 *   Variables: counter_a and counter_b at simple_benchmark.c
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

/* =========================================================================
 * The false sharing struct
 *
 * counter_a and counter_b are different variables written by different
 * threads, but they share the same 64-byte cache line.
 *
 * Memory layout (assuming struct is cache-line aligned):
 *   offset  0: counter_a  (8 bytes)  <- thread 0 writes this
 *   offset  8: counter_b  (8 bytes)  <- thread 1 writes this
 *   offset 16..63: unused
 *
 * Both fields sit within one 64-byte cache line, causing MESI
 * invalidation traffic on every write from either thread.
 * ========================================================================= */
struct Counters {
    uint64_t counter_a;   /* written only by the main thread  */
    uint64_t counter_b;   /* written only by the worker thread */
} counters;

/* How many times each thread increments its counter */
#define ITERATIONS 10000000

/* =========================================================================
 * Worker thread — writes counter_b
 * ========================================================================= */
void* worker(void* arg) {
    for (int i = 0; i < ITERATIONS; i++) {
        counters.counter_b++;
    }
    return NULL;
}

/* =========================================================================
 * Main — writes counter_a, spawns worker to write counter_b
 * ========================================================================= */
int main(void) {

    /* Print the addresses so you can cross-reference with PIN output */
    printf("=== Memory layout ===\n");
    printf("&counters.counter_a = %p  (cache line %p)\n",
           (void*)&counters.counter_a,
           (void*)((uintptr_t)&counters.counter_a & ~63ULL));
    printf("&counters.counter_b = %p  (cache line %p)\n",
           (void*)&counters.counter_b,
           (void*)((uintptr_t)&counters.counter_b & ~63ULL));
    printf("Both should show the SAME cache line address.\n\n");

    /* Spawn the worker thread */
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);

    /* Main thread writes counter_a while worker writes counter_b */
    for (int i = 0; i < ITERATIONS; i++) {
        counters.counter_a++;
    }

    /* Wait for worker to finish */
    pthread_join(thread, NULL);

    printf("counter_a = %llu\n", (unsigned long long)counters.counter_a);
    printf("counter_b = %llu\n", (unsigned long long)counters.counter_b);
    printf("Both should equal %d\n", ITERATIONS);

    return 0;
}
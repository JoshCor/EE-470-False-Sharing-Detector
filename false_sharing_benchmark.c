/*
 * false_sharing_benchmark.c
 *
 * A verbose toy benchmark demonstrating false sharing and its absence.
 * Designed to be used as ground truth validation for a PIN-based
 * false sharing detector.
 *
 * Four test scenarios:
 *   0 - FALSE SHARING:    threads write adjacent fields on the same cache line
 *   1 - NO FALSE SHARING: threads write fields on separate cache lines (padded)
 *   2 - PRODUCER/CONSUMER false sharing: head/tail of a queue on same cache line
 *   3 - TRUE SHARING:     threads write the exact same variable (data race,
 *                         NOT false sharing — your detector must distinguish this)
 *   4 - NO SHARING:       each thread has its own cache-line-aligned buffer
 *
 * Compile:
 *   gcc -O0 -g -pthread -o false_sharing_benchmark false_sharing_benchmark.c
 *
 * NOTE: -O0 is critical. Higher optimization levels may eliminate loops,
 * cache variables in registers, or reorder accesses — PIN would then see
 * fewer or no memory writes, making the benchmark useless for validation.
 *
 * Run:
 *   ./false_sharing_benchmark <scenario> <num_threads> <iterations>
 *
 * Example:
 *   ./false_sharing_benchmark 0 4 100000000
 *   ./false_sharing_benchmark 1 4 100000000
 *
 * Usage with PIN (once your tool is built):
 *   pin -t your_tool.so -- ./false_sharing_benchmark 0 4 50000000
 *   pin -t your_tool.so -- ./false_sharing_benchmark 1 4 50000000
 *
 * Expected results:
 *   Scenario 0: your tool should report a HIGH severity hotspot on the
 *               shared cache line containing all four counters.
 *   Scenario 1: your tool should report NO false sharing hotspots.
 *   Scenario 2: your tool should report a hotspot on the queue struct's
 *               cache line (head and tail fields).
 *   Scenario 3: your tool should NOT flag this as false sharing — it is
 *               a data race on a single variable, a different problem.
 *   Scenario 4: your tool should report NO false sharing hotspots.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

/*
 * x86 cache line size in bytes.
 * All modern x86 processors use 64-byte cache lines. This has been true
 * since the Pentium 4 and holds for all current Intel and AMD hardware.
 * You can verify on your machine:
 *   cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
 */
#define CACHE_LINE_SIZE 64

/*
 * Maximum threads this benchmark supports.
 * We size all arrays to this so we can use thread ID as an array index
 * without dynamic allocation — keeps the benchmark simple.
 */
#define MAX_THREADS 8

/*
 * Default workload parameters. Can be overridden via command line.
 */
#define DEFAULT_THREADS    4
#define DEFAULT_ITERATIONS 100000000  /* 100 million writes per thread */


/* =========================================================================
 * Scenario 0: FALSE SHARING
 *
 * Four uint64_t counters packed into a single struct.
 * Total size: 4 * 8 = 32 bytes — fits easily in one 64-byte cache line.
 *
 * Thread 0 writes c0, thread 1 writes c1, etc.
 * No thread ever reads another thread's counter.
 * But MESI sees only cache lines — so every write by any thread invalidates
 * the cache line for every other thread, forcing a cross-core fetch before
 * the next write can proceed.
 *
 * This is the canonical false sharing pattern.
 * ========================================================================= */
struct FalseSharingCounters {
    /*
     * All four fields sit within a single 64-byte cache line.
     * Typical layout on x86 (assuming struct starts on cache line boundary):
     *
     *   offset  0: c0  (8 bytes)
     *   offset  8: c1  (8 bytes)
     *   offset 16: c2  (8 bytes)
     *   offset 24: c3  (8 bytes)
     *   offset 32..63: unused padding from the compiler (or next struct fields)
     *
     * All four counters share one cache line. Any write by any thread
     * causes an invalidation visible to all other threads.
     */
    uint64_t c0;
    uint64_t c1;
    uint64_t c2;
    uint64_t c3;
};

/*
 * We use a global instance so its address is fixed and predictable.
 * This makes it easier to identify in your PIN tool's output.
 */
struct FalseSharingCounters fs_counters;


/* =========================================================================
 * Scenario 1: NO FALSE SHARING (padded)
 *
 * Same logical structure as scenario 0, but each counter is padded to
 * occupy its own full cache line. Now thread 0 owns cache line N,
 * thread 1 owns cache line N+1, etc. MESI never generates cross-thread
 * invalidations because no two threads touch the same cache line.
 *
 * This is the corrected version of scenario 0 and should be your negative
 * test case — your tool must report zero hotspots here.
 * ========================================================================= */
struct PaddedCounter {
    uint64_t value;
    /*
     * Padding to fill the rest of the cache line.
     * sizeof(uint64_t) = 8 bytes, so we need 64 - 8 = 56 bytes of padding
     * to push the next counter to the next cache line boundary.
     *
     * The modern C++ way is alignas(64), but since we're writing C here
     * we use explicit padding. Both achieve the same memory layout.
     */
    char pad[CACHE_LINE_SIZE - sizeof(uint64_t)];
};

/*
 * Array of padded counters.
 * padded_counters[0] is on cache line N
 * padded_counters[1] is on cache line N+1
 * padded_counters[2] is on cache line N+2
 * etc.
 *
 * We align the array itself to a cache line boundary to guarantee that
 * padded_counters[0] starts exactly at the beginning of a cache line,
 * not partway through one. Without this alignment, a counter that looks
 * padded correctly could still straddle two cache lines.
 */
struct PaddedCounter padded_counters[MAX_THREADS]
    __attribute__((aligned(CACHE_LINE_SIZE)));


/* =========================================================================
 * Scenario 2: PRODUCER/CONSUMER FALSE SHARING
 *
 * A simplified queue where the producer updates 'tail' and the consumer
 * updates 'head'. Both fields are in the same struct and therefore on
 * the same cache line. This is a very common real-world false sharing
 * pattern found in lock-free queues, ring buffers, and work queues.
 *
 * In a real program these threads would be doing meaningful work.
 * Here we just hammer the fields to make the false sharing visible.
 * ========================================================================= */
struct SharedQueue {
    /*
     * head: index of the next item to consume.
     * Written only by the consumer thread (thread 0).
     */
    uint64_t head;

    /*
     * tail: index of the next empty slot to produce into.
     * Written only by the producer thread (thread 1).
     */
    uint64_t tail;

    /*
     * Both head and tail occupy the first 16 bytes of this struct.
     * They share a cache line. The producer and consumer therefore
     * invalidate each other's cache on every write, even though they
     * never actually read each other's field.
     */
};

struct SharedQueue queue;


/* =========================================================================
 * Scenario 3: TRUE SHARING (NOT false sharing — a data race)
 *
 * Multiple threads write to the EXACT SAME variable.
 * This is a data race — a correctness problem, not a layout problem.
 * It is emphatically NOT false sharing.
 *
 * Your detector must not flag this as false sharing. The distinction:
 *
 *   False sharing:  threads A and B write DIFFERENT variables that happen
 *                   to occupy the SAME cache line. No logical dependency.
 *
 *   True sharing:   threads A and B write the SAME variable. There is a
 *                   logical dependency (or a bug). Cache coherence traffic
 *                   is the unavoidable cost of coordination (or the
 *                   symptom of a race condition).
 *
 * A real false sharing detector distinguishes these by checking whether
 * the exact addresses — not just the cache lines — being written overlap.
 * If two threads write to the exact same address, that's true sharing.
 * If they write to different addresses on the same cache line, that's
 * false sharing.
 * ========================================================================= */
uint64_t true_shared_counter = 0;


/* =========================================================================
 * Scenario 4: NO SHARING
 *
 * Each thread writes into its own cache-line-aligned buffer that no other
 * thread touches. There is zero cache coherence traffic between threads.
 * This is the ideal memory access pattern for independent parallel work.
 *
 * Your tool must report zero hotspots here.
 * ========================================================================= */

/*
 * Each thread gets its own 64-byte-aligned slot.
 * The __attribute__((aligned)) on the array guarantees the first element
 * starts on a cache line boundary. Each element is exactly CACHE_LINE_SIZE
 * bytes, so subsequent elements also start on cache line boundaries.
 *
 * no_share_buffers[0]: cache line N   (thread 0 only)
 * no_share_buffers[1]: cache line N+1 (thread 1 only)
 * no_share_buffers[2]: cache line N+2 (thread 2 only)
 * etc.
 */
typedef struct {
    uint64_t value;
    char     pad[CACHE_LINE_SIZE - sizeof(uint64_t)];
} CacheLineSlot;

CacheLineSlot no_share_buffers[MAX_THREADS]
    __attribute__((aligned(CACHE_LINE_SIZE)));


/* =========================================================================
 * Thread argument structure
 *
 * Passed to every worker thread. Contains everything the thread needs
 * to know: which scenario to run, which thread it is, and how many
 * iterations to perform.
 * ========================================================================= */
typedef struct {
    int      thread_id;   /* 0-indexed, used as array index and switch case */
    int      scenario;    /* which test to run */
    uint64_t iterations;  /* number of write operations to perform */
} ThreadArgs;


/* =========================================================================
 * Worker thread function
 *
 * All threads run this function. The scenario argument selects which
 * memory access pattern to exercise.
 * ========================================================================= */
void* worker(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    uint64_t i;

    switch (args->scenario) {

        /* -----------------------------------------------------------------
         * Scenario 0: FALSE SHARING
         * Each thread increments a different counter field, but all fields
         * share one cache line. MESI ping-pong is maximally expensive here.
         * ----------------------------------------------------------------- */
        case 0:
            for (i = 0; i < args->iterations; i++) {
                switch (args->thread_id) {
                    case 0: fs_counters.c0++; break;
                    case 1: fs_counters.c1++; break;
                    case 2: fs_counters.c2++; break;
                    case 3: fs_counters.c3++; break;
                    default:
                        /*
                         * Extra threads beyond 3 all write c3.
                         * This makes false sharing worse, not better —
                         * more threads fighting over the same cache line.
                         */
                        fs_counters.c3++;
                        break;
                }
            }
            break;

        /* -----------------------------------------------------------------
         * Scenario 1: NO FALSE SHARING (padded)
         * Each thread increments its own counter in its own cache line.
         * No cross-thread cache invalidations.
         * ----------------------------------------------------------------- */
        case 1:
            for (i = 0; i < args->iterations; i++) {
                padded_counters[args->thread_id].value++;
            }
            break;

        /* -----------------------------------------------------------------
         * Scenario 2: PRODUCER/CONSUMER FALSE SHARING
         * Thread 0 acts as consumer (updates head).
         * Thread 1 acts as producer (updates tail).
         * Both fields share a cache line despite being logically independent.
         * ----------------------------------------------------------------- */
        case 2:
            for (i = 0; i < args->iterations; i++) {
                if (args->thread_id == 0) {
                    queue.head++;  /* consumer advances head */
                } else {
                    queue.tail++;  /* producer advances tail */
                }
            }
            break;

        /* -----------------------------------------------------------------
         * Scenario 3: TRUE SHARING (data race — NOT false sharing)
         * All threads write to the exact same address.
         * This is a race condition that produces undefined behavior in C.
         * We include it specifically so your tool can demonstrate it does
         * NOT classify this as false sharing.
         *
         * Note: the result of true_shared_counter will be meaningless due
         * to the race. That's expected and irrelevant for our purposes.
         * ----------------------------------------------------------------- */
        case 3:
            for (i = 0; i < args->iterations; i++) {
                true_shared_counter++;  /* all threads, same address */
            }
            break;

        /* -----------------------------------------------------------------
         * Scenario 4: NO SHARING
         * Each thread writes only to its own cache-line-aligned slot.
         * Zero cross-thread cache coherence traffic.
         * ----------------------------------------------------------------- */
        case 4:
            for (i = 0; i < args->iterations; i++) {
                no_share_buffers[args->thread_id].value++;
            }
            break;

        default:
            fprintf(stderr, "Thread %d: unknown scenario %d\n",
                    args->thread_id, args->scenario);
            break;
    }

    return NULL;
}


/* =========================================================================
 * Timing helpers
 *
 * We time each scenario so you can observe the performance difference
 * between false sharing (slow) and no false sharing (fast).
 * The speedup ratio is your before/after validation metric.
 * ========================================================================= */
static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}


/* =========================================================================
 * Memory layout diagnostic
 *
 * Prints the addresses and cache line assignments of key variables.
 * This is your manual ground truth — you can verify your PIN tool's
 * reported cache line addresses against these values.
 * ========================================================================= */
static void print_memory_layout(void) {
    printf("\n=== Memory layout diagnostic ===\n");
    printf("Cache line size: %d bytes\n\n", CACHE_LINE_SIZE);

    /*
     * For each address, the cache line base is: addr & ~(CACHE_LINE_SIZE-1)
     * If two addresses have the same cache line base, they share a cache line.
     */
    printf("Scenario 0 — false sharing counters:\n");
    printf("  &fs_counters.c0 = %p  (cache line %p)\n",
           (void*)&fs_counters.c0,
           (void*)((uintptr_t)&fs_counters.c0 & ~(CACHE_LINE_SIZE-1)));
    printf("  &fs_counters.c1 = %p  (cache line %p)\n",
           (void*)&fs_counters.c1,
           (void*)((uintptr_t)&fs_counters.c1 & ~(CACHE_LINE_SIZE-1)));
    printf("  &fs_counters.c2 = %p  (cache line %p)\n",
           (void*)&fs_counters.c2,
           (void*)((uintptr_t)&fs_counters.c2 & ~(CACHE_LINE_SIZE-1)));
    printf("  &fs_counters.c3 = %p  (cache line %p)\n",
           (void*)&fs_counters.c3,
           (void*)((uintptr_t)&fs_counters.c3 & ~(CACHE_LINE_SIZE-1)));
    printf("  ^ all four should show the SAME cache line address\n\n");

    printf("Scenario 1 — padded counters:\n");
    for (int i = 0; i < 4; i++) {
        printf("  &padded_counters[%d].value = %p  (cache line %p)\n",
               i,
               (void*)&padded_counters[i].value,
               (void*)((uintptr_t)&padded_counters[i].value
                       & ~(CACHE_LINE_SIZE-1)));
    }
    printf("  ^ all four should show DIFFERENT cache line addresses\n\n");

    printf("Scenario 2 — queue head/tail:\n");
    printf("  &queue.head = %p  (cache line %p)\n",
           (void*)&queue.head,
           (void*)((uintptr_t)&queue.head & ~(CACHE_LINE_SIZE-1)));
    printf("  &queue.tail = %p  (cache line %p)\n",
           (void*)&queue.tail,
           (void*)((uintptr_t)&queue.tail & ~(CACHE_LINE_SIZE-1)));
    printf("  ^ both should show the SAME cache line address\n\n");

    printf("Scenario 3 — true sharing:\n");
    printf("  &true_shared_counter = %p  (cache line %p)\n",
           (void*)&true_shared_counter,
           (void*)((uintptr_t)&true_shared_counter & ~(CACHE_LINE_SIZE-1)));
    printf("  ^ single variable, all threads write same address\n\n");

    printf("Scenario 4 — no sharing:\n");
    for (int i = 0; i < 4; i++) {
        printf("  &no_share_buffers[%d].value = %p  (cache line %p)\n",
               i,
               (void*)&no_share_buffers[i].value,
               (void*)((uintptr_t)&no_share_buffers[i].value
                       & ~(CACHE_LINE_SIZE-1)));
    }
    printf("  ^ all four should show DIFFERENT cache line addresses\n\n");
}


/* =========================================================================
 * Main
 * ========================================================================= */
int main(int argc, char* argv[]) {

    /* Parse command line arguments */
    int      scenario   = (argc > 1) ? atoi(argv[1]) : 0;
    int      nthreads   = (argc > 2) ? atoi(argv[2]) : DEFAULT_THREADS;
    uint64_t iterations = (argc > 3) ? (uint64_t)atoll(argv[3])
                                     : DEFAULT_ITERATIONS;

    if (scenario < 0 || scenario > 4) {
        fprintf(stderr, "Usage: %s <scenario 0-4> [threads] [iterations]\n",
                argv[0]);
        fprintf(stderr, "  0: false sharing\n");
        fprintf(stderr, "  1: no false sharing (padded)\n");
        fprintf(stderr, "  2: producer/consumer false sharing\n");
        fprintf(stderr, "  3: true sharing (data race, not false sharing)\n");
        fprintf(stderr, "  4: no sharing\n");
        return 1;
    }

    if (nthreads < 1 || nthreads > MAX_THREADS) {
        fprintf(stderr, "Thread count must be between 1 and %d\n", MAX_THREADS);
        return 1;
    }

    /* Print memory layout so you can cross-reference with PIN output */
    print_memory_layout();

    /* Describe what we're about to run */
    const char* scenario_names[] = {
        "false sharing (scenario 0)",
        "no false sharing — padded (scenario 1)",
        "producer/consumer false sharing (scenario 2)",
        "true sharing — data race (scenario 3)",
        "no sharing (scenario 4)"
    };

    printf("=== Running: %s ===\n", scenario_names[scenario]);
    printf("Threads: %d  |  Iterations per thread: %llu\n\n",
           nthreads, (unsigned long long)iterations);

    /* Zero out all shared state before the run */
    memset(&fs_counters,       0, sizeof(fs_counters));
    memset(padded_counters,    0, sizeof(padded_counters));
    memset(&queue,             0, sizeof(queue));
    memset(&true_shared_counter, 0, sizeof(true_shared_counter));
    memset(no_share_buffers,   0, sizeof(no_share_buffers));

    /* Allocate thread handles and argument structs */
    pthread_t  threads[MAX_THREADS];
    ThreadArgs args[MAX_THREADS];

    /* Start timing */
    double t_start = now_seconds();

    /* Launch all threads */
    for (int i = 0; i < nthreads; i++) {
        args[i].thread_id  = i;
        args[i].scenario   = scenario;
        args[i].iterations = iterations;

        int rc = pthread_create(&threads[i], NULL, worker, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "Failed to create thread %d: error %d\n", i, rc);
            return 1;
        }
    }

    /* Wait for all threads to finish */
    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Stop timing */
    double elapsed = now_seconds() - t_start;

    /* Print results */
    printf("Elapsed time: %.3f seconds\n", elapsed);
    printf("Total writes: %llu\n",
           (unsigned long long)(iterations * (uint64_t)nthreads));
    printf("Writes/sec:   %.0f million\n\n",
           (double)(iterations * (uint64_t)nthreads) / elapsed / 1e6);

    /*
     * Print final counter values.
     * For scenario 0 and 1 the sum should equal iterations * nthreads
     * (since each thread increments a different counter exactly
     * 'iterations' times).
     *
     * For scenario 3 (true sharing / data race) the value will be less
     * than iterations * nthreads due to lost updates from the race.
     * This is expected and demonstrates why true sharing is a correctness
     * problem, not just a performance problem.
     */
    switch (scenario) {
        case 0:
            printf("Counter values (should sum to %llu):\n",
                   (unsigned long long)(iterations * (uint64_t)nthreads));
            printf("  c0=%llu  c1=%llu  c2=%llu  c3=%llu\n",
                   (unsigned long long)fs_counters.c0,
                   (unsigned long long)fs_counters.c1,
                   (unsigned long long)fs_counters.c2,
                   (unsigned long long)fs_counters.c3);
            break;
        case 1:
            printf("Counter values (each should equal %llu):\n",
                   (unsigned long long)iterations);
            for (int i = 0; i < nthreads; i++) {
                printf("  padded_counters[%d]=%llu\n",
                       i,
                       (unsigned long long)padded_counters[i].value);
            }
            break;
        case 2:
            printf("Queue state:\n");
            printf("  head=%llu  tail=%llu\n",
                   (unsigned long long)queue.head,
                   (unsigned long long)queue.tail);
            break;
        case 3:
            printf("true_shared_counter = %llu\n",
                   (unsigned long long)true_shared_counter);
            printf("Expected (no race):  %llu\n",
                   (unsigned long long)(iterations * (uint64_t)nthreads));
            printf("Lost updates due to race: %llu\n",
                   (unsigned long long)(iterations * (uint64_t)nthreads
                                        - true_shared_counter));
            break;
        case 4:
            printf("Thread-local buffer values (each should equal %llu):\n",
                   (unsigned long long)iterations);
            for (int i = 0; i < nthreads; i++) {
                printf("  no_share_buffers[%d]=%llu\n",
                       i,
                       (unsigned long long)no_share_buffers[i].value);
            }
            break;
    }

    /*
     * Reminder about what your PIN tool should report for this scenario.
     * These are the expected outputs you validate your detector against.
     */
    printf("\n=== Expected PIN tool output for this scenario ===\n");
    switch (scenario) {
        case 0:
            printf("HIGH severity hotspot on cache line containing fs_counters.\n");
            printf("4 writing threads, high invalidation rate.\n");
            printf("Variables: c0, c1, c2, c3 at false_sharing_benchmark.c\n");
            break;
        case 1:
            printf("NO hotspots. Zero false sharing detected.\n");
            printf("(If your tool reports anything here, it is a false positive.)\n");
            break;
        case 2:
            printf("MEDIUM severity hotspot on cache line containing queue.\n");
            printf("2 writing threads (producer and consumer).\n");
            printf("Variables: head, tail at false_sharing_benchmark.c\n");
            break;
        case 3:
            printf("NO false sharing hotspots.\n");
            printf("(True sharing on a single variable is a data race,\n");
            printf(" not false sharing. Your tool should not flag it.)\n");
            break;
        case 4:
            printf("NO hotspots. Zero false sharing detected.\n");
            printf("(If your tool reports anything here, it is a false positive.)\n");
            break;
    }
    printf("\n");

    return 0;
}
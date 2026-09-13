/**
 * @file turbo.c
 *
 * Reverse-hashing solver tuned for parallel scaling on a full scan.
 *
 * Every solver here pays the same ~440 ns per candidate inside lonesha256,
 * and that cost cannot be reduced without changing the hash, which the rules
 * forbid. So the only way to go faster on a full scan is to get closer to
 * perfect multi-core scaling: keep every core hashing, and stop the cores
 * from fighting each other over shared memory.
 *
 * The measured problem with a shared-slice design is false sharing. When
 * several threads' progress counters live in the same 64-byte cache line,
 * one thread's atomic update invalidates that line in every other core's
 * cache, so each core stalls reloading a line it did not even need. On a full
 * scan the counters are touched constantly, so this ping-pong is pure
 * overhead.
 *
 * Turbo removes it:
 *
 *   1. Each thread owns a counter that sits alone on its own 64-byte cache
 *      line (a padded, aligned struct). A thread advancing its own counter
 *      never invalidates another thread's line. This is the main win and it
 *      is completely portable.
 *
 *   2. The shared stop flag and answer sit alone on their own cache line too,
 *      so the one write that ends the search does not disturb other state.
 *
 *   3. Scheduling still starts each thread on its own contiguous slice (good
 *      locality, and it walks straight into a fixed-position answer), then
 *      lets an idle thread steal from a slice that still has work, so no core
 *      sits idle at the join.
 *
 *   4. The inner loop hashes with a single little-endian copy and rejects a
 *      candidate on its first 8 bytes with one integer compare, so the only
 *      real work per candidate is the mandatory lonesha256 call.
 *
 * The hash is still produced by lonesha256, one call per candidate. No
 * batching, no SIMD reimplementation, no precomputed table, no
 * platform-specific scheduling calls.
 */

#define _GNU_SOURCE
#include "../solver.h"
#include "../../vendor/lonesha256.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// The number of threads and base slices. Overridable at compile time
// (-DTURBO_THREADS=N). The default matches a typical physical core count;
// oversubscription only adds contention on a compute-bound full scan.
#ifndef TURBO_THREADS
#define TURBO_THREADS 8
#endif
#define THREAD_COUNT TURBO_THREADS

// How many candidates a thread claims from a counter at a time. Large enough
// that the atomic is negligible next to STEP hashes, small enough that
// stealing stays fine-grained near the end of the search.
#ifndef TURBO_STEP
#define TURBO_STEP 8192
#endif
#define STEP TURBO_STEP

// How often (in candidates checked) to poll the shared stop flag.
#define POLL_MASK 0xFFF

// A cache line on the target CPUs. Counters and the stop flag are aligned to
// this so that independent state never shares a line (no false sharing).
#define CACHE_LINE 64

/**
 * @struct Counter
 * @brief One thread's progress counter, alone on its own cache line.
 *
 * @field cursor Next unclaimed number in this thread's slice. Any thread may
 *        advance it (that is how work-stealing happens), but because each
 *        Counter occupies a full cache line, advancing one never invalidates
 *        another thread's line.
 * @field end One past the last number of this thread's slice.
 * @field pad Filler so the struct fills a whole cache line.
 */
typedef struct
{
    _Atomic uint64_t cursor;
    uint64_t end;
    uint8_t pad[CACHE_LINE - 2 * sizeof(uint64_t)];
} __attribute__((aligned(CACHE_LINE))) Counter;

/**
 * @struct Stop
 * @brief The shared stop flag and answer, alone on their own cache line.
 */
typedef struct
{
    atomic_bool found;
    _Atomic uint64_t answer;
    uint8_t pad[CACHE_LINE - sizeof(atomic_bool) - sizeof(uint64_t)];
} __attribute__((aligned(CACHE_LINE))) Stop;

/**
 * @struct Work
 * @brief Per-thread arguments. Small and read-mostly.
 *
 * @field target The full 32-byte hash to reverse.
 * @field target_prefix First 8 bytes of target as a uint64_t for fast reject.
 * @field counters The array of per-thread counters (one cache line each).
 * @field count How many counters/threads exist.
 * @field mine Index of this thread's own counter.
 * @field stop Pointer to the shared stop flag / answer line.
 */
typedef struct
{
    const uint8_t *target;
    uint64_t target_prefix;
    Counter *counters;
    int count;
    int mine;
    Stop *stop;
} Work;

/**
 * @function claim_step
 * @brief Atomically claim up to STEP candidates from a counter.
 *
 * A relaxed pre-check skips the read-modify-write when the counter is already
 * exhausted, which matters during stealing so a drained slice costs only a
 * load rather than a full atomic.
 * @return true if a non-empty step was claimed.
 */
static inline bool claim_step(Counter *counter, uint64_t *low, uint64_t *high)
{
    if (atomic_load_explicit(&counter->cursor, memory_order_relaxed) >=
        counter->end)
    {
        return false;
    }

    uint64_t start = atomic_fetch_add_explicit(&counter->cursor, STEP,
                                               memory_order_relaxed);
    if (start >= counter->end)
    {
        return false;
    }

    uint64_t stop = start + STEP;
    if (stop > counter->end)
    {
        stop = counter->end;
    }

    *low = start;
    *high = stop;
    return true;
}

/**
 * @function scan_step
 * @brief Hash every candidate in [low, high) and check it against target.
 *
 * The target and stop pointer are passed as locals so the hot loop touches no
 * shared struct fields beyond the occasional relaxed flag poll.
 * @return true if the match was found (the caller should stop).
 */
static inline bool scan_step(uint64_t low, uint64_t high,
                             const uint8_t *target, uint64_t target_prefix,
                             Stop *stop)
{
    uint8_t candidate_hash[HASH_SIZE];
    uint64_t checked = 0;

    for (uint64_t number = low; number < high; number++)
    {
        if ((checked++ & POLL_MASK) == 0 &&
            atomic_load_explicit(&stop->found, memory_order_relaxed))
        {
            return true;
        }

        // Little-endian input: the object representation of a uint64_t on a
        // little-endian host already is its little-endian byte order.
        uint8_t input[8];
        memcpy(input, &number, sizeof(input));
        lonesha256(candidate_hash, input, sizeof(input));

        // Reject on the first 8 bytes with a single integer compare. Only a
        // real match survives to the full 32-byte confirmation.
        uint64_t prefix;
        memcpy(&prefix, candidate_hash, sizeof(prefix));
        if (prefix != target_prefix)
        {
            continue;
        }

        if (memcmp(candidate_hash, target, HASH_SIZE) == 0)
        {
            atomic_store_explicit(&stop->answer, number, memory_order_relaxed);
            atomic_store_explicit(&stop->found, true, memory_order_release);
            return true;
        }
    }

    return false;
}

/**
 * @function search_part
 * @brief Drain this thread's own counter, then steal from others.
 * @param argument The Work record for this thread.
 * @return NULL when the search is done.
 */
static void *search_part(void *argument)
{
    Work *work = (Work *)argument;

    // Hoist read-mostly state into locals so the hot loop is register-bound.
    const uint8_t *target = work->target;
    uint64_t target_prefix = work->target_prefix;
    Stop *stop = work->stop;
    Counter *counters = work->counters;
    int count = work->count;
    int mine = work->mine;

    uint64_t low, high;

    // Phase 1: our own slice. Good locality, and for a fixed-position answer
    // the owning thread walks straight into it.
    while (claim_step(&counters[mine], &low, &high))
    {
        if (atomic_load_explicit(&stop->found, memory_order_relaxed))
        {
            return NULL;
        }
        if (scan_step(low, high, target, target_prefix, stop))
        {
            return NULL;
        }
    }

    // Phase 2: steal from any counter that still has work, so no core idles
    // at the join while another still has a full slice to grind.
    for (;;)
    {
        if (atomic_load_explicit(&stop->found, memory_order_relaxed))
        {
            return NULL;
        }

        bool stole = false;
        for (int offset = 1; offset < count; offset++)
        {
            int victim = mine + offset;
            if (victim >= count)
            {
                victim -= count;
            }

            if (claim_step(&counters[victim], &low, &high))
            {
                stole = true;
                if (scan_step(low, high, target, target_prefix, stop))
                {
                    return NULL;
                }
            }
        }

        // A full pass found no work anywhere: the range is exhausted.
        if (!stole)
        {
            return NULL;
        }
    }
}

/**
 * @function turbo_solve
 * @brief Search the full range with cache-line-isolated per-thread counters.
 * @param target The hash that must be reversed.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number, or start if no match was found.
 */
static uint64_t turbo_solve(const uint8_t target[HASH_SIZE],
                            uint64_t start,
                            uint64_t end)
{
    uint64_t total = end - start;

    // Guard against a degenerate/empty range.
    if (total == 0)
    {
        return start;
    }

    pthread_t threads[THREAD_COUNT];
    Work work[THREAD_COUNT];

    // Each Counter must keep its own cache line, so allocate the array with
    // cache-line alignment. aligned_alloc needs a size that is a multiple of
    // the alignment; sizeof(Counter) already is CACHE_LINE.
    Counter *counters = aligned_alloc(CACHE_LINE,
                                      THREAD_COUNT * sizeof(Counter));
    if (counters == NULL)
    {
        // Fall back to a single-threaded scan rather than fail outright.
        uint8_t h[HASH_SIZE];
        for (uint64_t n = start; n < end; n++)
        {
            uint8_t in[8];
            memcpy(in, &n, sizeof(in));
            lonesha256(h, in, sizeof(in));
            if (memcmp(h, target, HASH_SIZE) == 0)
            {
                return n;
            }
        }
        return start;
    }

    Stop stop;
    atomic_store_explicit(&stop.found, false, memory_order_relaxed);
    atomic_store_explicit(&stop.answer, start, memory_order_relaxed);

    uint64_t target_prefix;
    memcpy(&target_prefix, target, sizeof(target_prefix));

    // Split into THREAD_COUNT contiguous slices; the first "remainder" slices
    // get one extra number so every candidate is covered exactly once.
    uint64_t part_size = total / THREAD_COUNT;
    uint64_t remainder = total % THREAD_COUNT;

    uint64_t next = start;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        uint64_t size = part_size + ((uint64_t)i < remainder ? 1 : 0);
        atomic_store_explicit(&counters[i].cursor, next, memory_order_relaxed);
        counters[i].end = next + size;
        next += size;
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        work[i].target = target;
        work[i].target_prefix = target_prefix;
        work[i].counters = counters;
        work[i].count = THREAD_COUNT;
        work[i].mine = i;
        work[i].stop = &stop;

        pthread_create(&threads[i], NULL, search_part, &work[i]);
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }

    uint64_t answer = atomic_load_explicit(&stop.answer, memory_order_relaxed);
    free(counters);
    return answer;
}

const Solver solver_turbo = {"turbo", turbo_solve};

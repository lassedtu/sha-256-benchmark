/**
 * @file dynamic.c
 *
 * Reverse-hashing solver that uses many threads with dynamic scheduling.
 *
 * The range is split into many small chunks (more than there are threads).
 * Threads pull chunks from a shared counter as they finish their current
 * one, so no thread sits idle while another still has a full static slice
 * left. Each candidate is hashed with a single call to lonesha256; because
 * that call is external and cannot be inlined, hashing several candidates
 * "in parallel" would only add overhead, so the inner loop stays tight.
 */

#define _GNU_SOURCE
#include "../solver.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

// The number of threads.
#define THREAD_COUNT 16

// Chunks per thread. More chunks than threads means a thread that finishes
// early can steal more work instead of waiting at pthread_join.
#define CHUNKS_PER_THREAD 8
#define CHUNK_COUNT (THREAD_COUNT * CHUNKS_PER_THREAD)

// How often (in candidates checked within a chunk) to poll the shared
// "found" flag for early exit.
#define POLL_MASK 0xFFF

/**
 * @struct Work
 * @brief The shared state that every thread reads from and pulls work from.
 *
 * @field target The hash to reverse.
 * @field range_start The first number of the whole search range.
 * @field chunk_size The size of one chunk, in numbers.
 * @field total The size of the whole search range, in numbers.
 * @field next_chunk Shared counter. Each thread atomically claims the next
 *        chunk index and searches it.
 * @field found The shared flag. It is true after a match.
 * @field answer The shared place for the answer.
 */
typedef struct
{
    const uint8_t *target;
    uint64_t range_start;
    uint64_t chunk_size;
    uint64_t total;
    _Atomic uint64_t *next_chunk;
    atomic_bool *found;
    _Atomic uint64_t *answer;
} Work;

/**
 * @function search_range
 * @brief Search [low, high) for the target hash.
 *
 * A tight loop over a single 32-byte buffer. lonesha256 is an external,
 * non-inlinable call, so hashing several candidates "in parallel" cannot
 * overlap their latencies; it only adds stack traffic and a second compare
 * loop. Hashing one candidate at a time into a hot buffer is faster.
 * @return true if the match was found in this range.
 */
static inline bool search_range(uint64_t low, uint64_t high,
                                const uint8_t *target,
                                atomic_bool *found,
                                _Atomic uint64_t *answer)
{
    uint8_t candidate_hash[HASH_SIZE];
    uint64_t checked = 0;

    for (uint64_t number = low; number < high; number++)
    {
        // Poll the shared flag occasionally so a match elsewhere stops us.
        if ((checked++ & POLL_MASK) == 0 &&
            atomic_load_explicit(found, memory_order_relaxed))
        {
            return true;
        }

        hash_of_number(number, candidate_hash);
        if (hash_equal(candidate_hash, target))
        {
            atomic_store_explicit(answer, number, memory_order_relaxed);
            atomic_store_explicit(found, true, memory_order_release);
            return true;
        }
    }

    return false;
}

/**
 * @function search_part
 * @brief Pull chunks from the shared counter and search each one, until the
 *        range is exhausted or another thread reports a match.
 * @param argument The Work record (shared by all threads).
 * @return NULL when the search is done.
 */
static void *search_part(void *argument)
{
    Work *work = (Work *)argument;

    for (;;)
    {
        if (atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return NULL;
        }

        uint64_t index = atomic_fetch_add_explicit(work->next_chunk, 1,
                                                   memory_order_relaxed);
        uint64_t chunk_start = index * work->chunk_size;
        if (chunk_start >= work->total)
        {
            return NULL; // No chunks left.
        }

        uint64_t chunk_end = chunk_start + work->chunk_size;
        if (chunk_end > work->total || index == CHUNK_COUNT - 1)
        {
            chunk_end = work->total; // Last chunk absorbs the remainder.
        }

        uint64_t low = work->range_start + chunk_start;
        uint64_t high = work->range_start + chunk_end;

        if (search_range(low, high, work->target, work->found, work->answer))
        {
            return NULL;
        }
    }
}

/**
 * @function pin_thread
 * @brief Best-effort pin a thread to a specific CPU core.
 *
 * Improves cache locality by preventing the scheduler from migrating a
 * thread mid-search. Failure is not fatal: the search is still correct
 * without pinning, just potentially a little slower.
 */
static void pin_thread(pthread_t thread, int cpu)
{
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
#else
    // CPU affinity (cpu_set_t / pthread_setaffinity_np) is a Linux/glibc
    // extension and is unavailable on other platforms such as macOS.
    // Pinning is best-effort, so skip it here without affecting correctness.
    (void)thread;
    (void)cpu;
#endif
}

/**
 * @function dynamic_solve
 * @brief Search the full range with multiple threads, using work-stealing
 *        chunks so no thread finishes early and sits idle.
 * @param target The hash that must be reversed.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number, or start if no match was found.
 */
static uint64_t dynamic_solve(const uint8_t target[HASH_SIZE],
                              uint64_t start,
                              uint64_t end)
{
    pthread_t threads[THREAD_COUNT];

    atomic_bool found = false;
    _Atomic uint64_t answer = start;
    _Atomic uint64_t next_chunk = 0;

    uint64_t total = end - start;

    // Guard against a degenerate/empty range.
    if (total == 0)
    {
        return start;
    }

    uint64_t chunk_size = total / CHUNK_COUNT;
    if (chunk_size == 0)
    {
        chunk_size = 1;
    }

    Work work = {
        .target = target,
        .range_start = start,
        .chunk_size = chunk_size,
        .total = total,
        .next_chunk = &next_chunk,
        .found = &found,
        .answer = &answer,
    };

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_create(&threads[i], NULL, search_part, &work);
        pin_thread(threads[i], i);
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }

    return atomic_load_explicit(&answer, memory_order_relaxed);
}

const Solver solver_dynamic = {"dynamic", dynamic_solve};
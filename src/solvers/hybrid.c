/**
 * @file hybrid.c
 *
 * Reverse-hashing solver that combines static partitioning with
 * work-stealing.
 *
 * The range is split into THREAD_COUNT contiguous base slices, one per
 * thread. A thread first scans its own slice. This keeps the strength of
 * the purely static "threaded" solver: when the answer sits at a fixed
 * position (e.g. the last quarter of the range) the thread that owns that
 * region walks straight into it, and the others stop at their next poll.
 *
 * The extra ingredient is stealing. Each slice is consumed through its own
 * atomic cursor, in fine-grained steps. When a thread exhausts its own
 * slice it looks at every other slice and steals a step from whichever one
 * still has work left. So no core sits idle at pthread_join while another
 * core still has a full slice to grind through. That is the case where the
 * static "threaded" solver wastes time and this one does not.
 *
 * Each candidate is hashed with a single call to lonesha256. That call is
 * external and cannot be inlined, so hashing several candidates "in
 * parallel" would only add overhead; the inner loop stays tight.
 */

#define _GNU_SOURCE
#include "../solver.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

// The number of threads, and therefore the number of base slices.
#define THREAD_COUNT 16

// How many candidates a thread claims from a slice cursor at a time. Large
// enough that the atomic fetch-add is negligible next to the hashing, small
// enough that stealing stays fine-grained near the end of the search.
#define STEP 4096

// How often (in candidates checked) to poll the shared "found" flag.
#define POLL_MASK 0xFFF

/**
 * @struct Slice
 * @brief One base slice and its progress cursor.
 *
 * @field base The first number of this slice.
 * @field end One past the last number of this slice.
 * @field cursor Next unclaimed number in this slice. Threads atomically
 *        advance it by STEP to claim a step of work. Any thread may advance
 *        it, which is what makes stealing possible.
 */
typedef struct
{
    uint64_t base;
    uint64_t end;
    _Atomic uint64_t cursor;
} Slice;

/**
 * @struct Work
 * @brief Shared state for every thread.
 *
 * @field target The hash to reverse.
 * @field slices The per-thread base slices.
 * @field slice_count How many slices exist (== THREAD_COUNT).
 * @field my_slice The index of this thread's own base slice.
 * @field found Shared flag, true after a match.
 * @field answer Shared place for the answer.
 */
typedef struct
{
    const uint8_t *target;
    Slice *slices;
    int slice_count;
    int my_slice;
    atomic_bool *found;
    _Atomic uint64_t *answer;
} Work;

/**
 * @function claim_step
 * @brief Atomically claim up to STEP candidates from one slice.
 * @param slice The slice to claim from.
 * @param low Output: first number of the claimed step.
 * @param high Output: one past the last number of the claimed step.
 * @return true if a non-empty step was claimed, false if the slice is done.
 */
static inline bool claim_step(Slice *slice, uint64_t *low, uint64_t *high)
{
    uint64_t start = atomic_fetch_add_explicit(&slice->cursor, STEP,
                                               memory_order_relaxed);
    if (start >= slice->end)
    {
        return false;
    }

    uint64_t stop = start + STEP;
    if (stop > slice->end)
    {
        stop = slice->end;
    }

    *low = start;
    *high = stop;
    return true;
}

/**
 * @function scan_step
 * @brief Hash every candidate in [low, high) and check it against target.
 * @return true if the match was found in this step.
 */
static inline bool scan_step(uint64_t low, uint64_t high,
                             const uint8_t *target,
                             atomic_bool *found,
                             _Atomic uint64_t *answer)
{
    uint8_t candidate_hash[HASH_SIZE];
    uint64_t checked = 0;

    for (uint64_t number = low; number < high; number++)
    {
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
 * @brief Scan this thread's own slice first, then steal from other slices.
 * @param argument The Work record for this thread.
 * @return NULL when the search is done.
 */
static void *search_part(void *argument)
{
    Work *work = (Work *)argument;
    uint64_t low, high;

    // Phase 1: drain this thread's own base slice. Starting on the slice we
    // own gives good locality and, for fixed-position answers, sends the
    // owning thread straight at the region that holds the answer.
    Slice *mine = &work->slices[work->my_slice];
    while (claim_step(mine, &low, &high))
    {
        if (atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return NULL;
        }
        if (scan_step(low, high, work->target, work->found, work->answer))
        {
            return NULL;
        }
    }

    // Phase 2: our slice is empty. Steal from any slice that still has work,
    // so we do not sit idle while another thread still has a full slice.
    for (;;)
    {
        if (atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return NULL;
        }

        bool stole = false;
        for (int offset = 1; offset < work->slice_count; offset++)
        {
            int victim = (work->my_slice + offset) % work->slice_count;
            if (claim_step(&work->slices[victim], &low, &high))
            {
                stole = true;
                if (scan_step(low, high, work->target, work->found,
                              work->answer))
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
 * @function pin_thread
 * @brief Best-effort pin a thread to a specific CPU core.
 *
 * Improves cache locality by preventing the scheduler from migrating a
 * thread mid-search. Failure is not fatal: the search stays correct.
 */
static void pin_thread(pthread_t thread, int cpu)
{
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
#else
    // CPU affinity is a Linux/glibc extension, unavailable on macOS. Pinning
    // is best-effort, so skip it here without affecting correctness.
    (void)thread;
    (void)cpu;
#endif
}

/**
 * @function hybrid_solve
 * @brief Search the full range: static base slices plus work-stealing.
 * @param target The hash that must be reversed.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number, or start if no match was found.
 */
static uint64_t hybrid_solve(const uint8_t target[HASH_SIZE],
                             uint64_t start,
                             uint64_t end)
{
    pthread_t threads[THREAD_COUNT];
    Work work[THREAD_COUNT];
    Slice slices[THREAD_COUNT];

    atomic_bool found = false;
    _Atomic uint64_t answer = start;

    uint64_t total = end - start;

    // Guard against a degenerate/empty range.
    if (total == 0)
    {
        return start;
    }

    // Split the range into THREAD_COUNT contiguous base slices. The first
    // "remainder" slices get one extra number so every candidate is covered.
    uint64_t part_size = total / THREAD_COUNT;
    uint64_t remainder = total % THREAD_COUNT;

    uint64_t next = start;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        uint64_t size = part_size + ((uint64_t)i < remainder ? 1 : 0);
        slices[i].base = next;
        slices[i].end = next + size;
        atomic_store_explicit(&slices[i].cursor, next, memory_order_relaxed);
        next += size;
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        work[i].target = target;
        work[i].slices = slices;
        work[i].slice_count = THREAD_COUNT;
        work[i].my_slice = i;
        work[i].found = &found;
        work[i].answer = &answer;

        pthread_create(&threads[i], NULL, search_part, &work[i]);
        pin_thread(threads[i], i);
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }

    return atomic_load_explicit(&answer, memory_order_relaxed);
}

const Solver solver_hybrid = {"hybrid", hybrid_solve};

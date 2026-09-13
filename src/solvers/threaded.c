/**
 * @file threaded.c
 *
 * Reverse-hashing solver that uses many threads.
 *
 * The range is split into parts. Each thread searches one part at the same time.
 */

#include "../solver.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

// The number of threads.
#define THREAD_COUNT 16

/**
 * @struct Work
 * @brief The data that one thread needs for its part of the search.
 *
 * @field target The hash to reverse.
 * @field start The first number of this part.
 * @field end One past the last number of this part.
 * @field found The shared flag. It is true after a match.
 * @field answer The shared place for the answer.
 */
typedef struct
{
    const uint8_t *target;
    uint64_t start;
    uint64_t end;
    atomic_bool *found;
    _Atomic uint64_t *answer;
} Work;

/**
 * @function search_part
 * @brief Search one part of the range.
 *
 * The thread checks the shared flag every so often. If another thread found the
 * answer, this thread stops early.
 * @param argument The Work record for this thread.
 * @return NULL when the search is done.
 */
static void *search_part(void *argument)
{
    Work *work = (Work *)argument;
    uint8_t candidate_hash[HASH_SIZE];

    for (uint64_t number = work->start; number < work->end; number++)
    {
        // Look at the shared flag once every 4096 numbers.
        if ((number & 0xFFF) == 0 && atomic_load(work->found))
        {
            return NULL;
        }

        hash_of_number(number, candidate_hash);

        if (hash_equal(candidate_hash, work->target))
        {
            atomic_store(work->answer, number);
            atomic_store(work->found, true);
            return NULL;
        }
    }

    return NULL;
}

/**
 * @function threaded_solve
 * @brief Search the full range with multiple threads.
 *
 * The range is split into parts. Each thread searches one part at the same time.
 * @param target The hash that must be reversed.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number, or start if no match was found.
 */
static uint64_t threaded_solve(const uint8_t target[HASH_SIZE],
                               uint64_t start,
                               uint64_t end)
{
    pthread_t threads[THREAD_COUNT];
    Work work[THREAD_COUNT];

    atomic_bool found = false;
    _Atomic uint64_t answer = start;

    uint64_t total = end - start;

    // If the range is very small, one thread is enough.
    uint64_t part_size = total / THREAD_COUNT;
    uint64_t remainder = total % THREAD_COUNT;

    uint64_t next = start;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        uint64_t size = part_size;
        // Give the first remainder threads one extra number.
        if ((uint64_t)i < remainder)
        {
            size += 1;
        }

        work[i].target = target;
        work[i].start = next;
        work[i].end = next + size;
        work[i].found = &found;
        work[i].answer = &answer;

        next += size;

        pthread_create(&threads[i], NULL, search_part, &work[i]);
    }

    // Wait for all threads to finish.
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }

    return atomic_load(&answer);
}

const Solver solver_threaded = {"threaded", threaded_solve};

/**
 * @file linear.c
 *
 * Simple reverse-hashing solver that scans the range in order.
 */

#include "../solver.h"

/**
 * @function linear_solve
 * @brief Search the range from start to end.
 *
 * The solver checks each number in order and returns the first one whose hash
 * matches the target.
 * @param target The hash that must be reversed.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number, or start if no match was found.
 */
static uint64_t linear_solve(const uint8_t target[HASH_SIZE],
                             uint64_t start,
                             uint64_t end)
{
    uint8_t candidate_hash[HASH_SIZE];

    for (uint64_t number = start; number < end; number++)
    {
        // Make the hash of the current number.
        hash_of_number(number, candidate_hash);

        // Compare the hash with the target hash.
        if (hash_equal(candidate_hash, target))
        {
            return number;
        }
    }

    // The algorithm found no match. Return start as a safe default.
    return start;
}

// This record makes the algorithm visible to the registry.
const Solver solver_linear = {"linear", linear_solve};

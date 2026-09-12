/**
 * @file solver.h
 *
 * Interface for a reverse-hashing solver.
 *
 * A solver is one algorithm that searches a range. It gets a target hash and a
 * range of numbers, then returns the number whose hash matches the target.
 */

#ifndef SOLVER_H
#define SOLVER_H

#include <stdint.h>
#include "hash.h"

/**
 * @typedef solver_fn
 * @brief The type of a reverse-hashing function.
 *
 * @param target The hash that must be reversed. It has 32 bytes.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number. If no match is found, return start.
 */
typedef uint64_t (*solver_fn)(const uint8_t target[HASH_SIZE],
                              uint64_t start,
                              uint64_t end);

/**
 * @struct Solver
 * @brief One solver name and its search function.
 *
 * @field name A short text that identifies the solver.
 * @field run The function that does the search.
 */
typedef struct
{
    const char *name;
    solver_fn run;
} Solver;

#endif // SOLVER_H

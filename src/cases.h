/**
 * @file cases.h
 *
 * Preset test cases for the benchmark.
 *
 * A test case is one preset problem for the solvers. Each case has a known
 * answer inside a known range. The benchmark hashes that answer and then asks
 * each solver to find it again.
 */

#ifndef CASES_H
#define CASES_H

#include <stdint.h>

/**
 * @struct TestCase
 * @brief One preset problem for the solvers.
 *
 * @field answer The number that the solver must find.
 * @field start The first number of the search range.
 * @field difficulty The size of the range. The range is [start, start + difficulty).
 * @field label A short text that identifies the case in the benchmark output.
 *
 * The answer must stay inside the range [start, start + difficulty).
 */
typedef struct
{
    uint64_t answer;
    uint64_t start;
    uint64_t difficulty;
    const char *label;
} TestCase;

// preset_cases points to the first test case in the array in cases.c.
// preset_count is the number of test cases in that array.
extern const TestCase *const preset_cases;
extern const int preset_count;

#endif // CASES_H

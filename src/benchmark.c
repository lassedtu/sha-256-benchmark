/**
 * @file benchmark.c
 *
 * Benchmark harness for comparing reverse-hashing solvers.
 *
 * The program runs a set of preset test cases. For each case, it hashes the
 * known answer, gives the hash and range to a solver, and checks the result.
 * It then prints the average time for each solver.
 */

#include "registry.h"
#include "hash.h"
#include "cases.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/**
 * @function seconds_between
 * @brief Get the time in seconds between two time points.
 *
 * This uses a monotonic clock, so it isn't affected by system clock changes.
 * @param begin The start time.
 * @param finish The end time.
 * @return The elapsed time in seconds.
 */
static double seconds_between(struct timespec begin, struct timespec finish)
{
    double seconds = (double)(finish.tv_sec - begin.tv_sec);
    double nanoseconds = (double)(finish.tv_nsec - begin.tv_nsec);
    return seconds + nanoseconds / 1e9;
}

/**
 * @function run_one_solver
 * @brief Measure one solver over the selected test cases.
 *
 * The max_difficulty filter skips cases that are too hard.
 * @param solver The solver to test.
 * @param repeat How many times to run each selected case.
 * @param max_difficulty The highest allowed difficulty. Use 0 to run all cases.
 * @param ok Set to 1 if all answers are correct, otherwise 0.
 * @return The average time in seconds per case.
 */
static double run_one_solver(const Solver *solver, int repeat,
                             uint64_t max_difficulty, int *ok)
{
    double total_time = 0.0;
    int total_runs = 0;
    *ok = 1;

    for (int c = 0; c < preset_count; c++)
    {
        const TestCase *test = &preset_cases[c];

        // Skip a case that is harder than the filter allows.
        if (max_difficulty != 0 && test->difficulty > max_difficulty)
        {
            continue;
        }

        // Make the target hash from the known answer of this test case.
        uint8_t target[HASH_SIZE];
        hash_of_number(test->answer, target);

        uint64_t range_end = test->start + test->difficulty;

        for (int r = 0; r < repeat; r++)
        {
            struct timespec begin, finish;

            clock_gettime(CLOCK_MONOTONIC, &begin);
            uint64_t result = solver->run(target, test->start, range_end);
            clock_gettime(CLOCK_MONOTONIC, &finish);

            total_time += seconds_between(begin, finish);
            total_runs += 1;

            // Check that the solver found the correct number.
            if (result != test->answer)
            {
                printf("  [WRONG] solver \"%s\" case \"%s\": "
                       "expected %llu but got %llu\n",
                       solver->name, test->label,
                       (unsigned long long)test->answer,
                       (unsigned long long)result);
                *ok = 0;
            }
        }
    }

    if (total_runs == 0)
    {
        return 0.0;
    }
    return total_time / (double)total_runs;
}

/**
 * @function main
 * @brief Run the benchmark.
 *
 * The program reads optional command-line arguments for repeat count and
 * difficulty filter, then runs the registered solvers and prints a result table.
 * @param argc The number of command-line arguments.
 * @param argv The command-line arguments.
 * @return 0 on success.
 */
int main(int argc, char **argv)
{
    int repeat = 1;
    if (argc >= 2)
    {
        repeat = atoi(argv[1]);
        if (repeat < 1)
        {
            repeat = 1;
        }
    }

    uint64_t max_difficulty = 0;
    if (argc >= 3)
    {
        max_difficulty = strtoull(argv[2], NULL, 10);
    }

    // Count how many cases the filter selects, so the header is honest.
    int selected = 0;
    for (int c = 0; c < preset_count; c++)
    {
        if (max_difficulty == 0 || preset_cases[c].difficulty <= max_difficulty)
        {
            selected += 1;
        }
    }

    printf("Reverse-hashing benchmark\n");
    if (max_difficulty == 0)
    {
        printf("Test cases: %d   Repeat per case: %d\n\n", selected, repeat);
    }
    else
    {
        printf("Test cases: %d (of %d, max difficulty %llu)   "
               "Repeat per case: %d\n\n",
               selected, preset_count,
               (unsigned long long)max_difficulty, repeat);
    }

    // Print the header of the result table.
    printf("%-12s %18s %12s\n", "solver", "avg time/case (s)", "correct");
    printf("---------------------------------------------------\n");

    // Track the fastest correct solver, so we can name a winner.
    const char *fastest_name = NULL;
    double fastest_time = 0.0;

    for (int s = 0; s < solver_count; s++)
    {
        const Solver *solver = solver_registry[s];

        int ok = 0;
        double average = run_one_solver(solver, repeat, max_difficulty, &ok);

        printf("%-12s %18.6f %12s\n",
               solver->name, average, ok ? "yes" : "NO");
        // Flush the line at once. Thus a slow run still shows progress.
        fflush(stdout);

        if (ok)
        {
            if (fastest_name == NULL || average < fastest_time)
            {
                fastest_name = solver->name;
                fastest_time = average;
            }
        }
    }

    printf("---------------------------------------------------\n");
    if (fastest_name != NULL)
    {
        printf("Fastest correct solver: \"%s\" (%.6f s per case)\n",
               fastest_name, fastest_time);
    }
    else
    {
        printf("No solver gave correct answers on all test cases.\n");
    }

    return 0;
}

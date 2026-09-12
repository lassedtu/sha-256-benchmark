/**
 * @file benchmark.c
 *
 * Benchmark harness for comparing reverse-hashing solvers.
 *
 * The program runs a set of preset test cases. For each case, it hashes the
 * known answer, gives the hash and range to a solver, and checks the result.
 * As each test case finishes, it prints which case ran, which solver ran it,
 * and how long it took. When every solver is done, it prints a single ranking
 * from slowest to fastest, including the percentage improvement each solver
 * gives over the next-slower one.
 */

#include "registry.h"
#include "hash.h"
#include "cases.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/**
 * @struct SolverResult
 * @brief The measured outcome for one solver.
 *
 * @field name The solver name.
 * @field average The average time in seconds per case.
 * @field ok Whether the solver answered every case correctly.
 */
typedef struct
{
    const char *name;
    double average;
    int ok;
} SolverResult;

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
 * After every case runs, this prints a progress line naming the case, the
 * solver, and the time it took. The max_difficulty filter skips cases that are
 * too hard.
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

            double elapsed = seconds_between(begin, finish);
            total_time += elapsed;
            total_runs += 1;

            // Check that the solver found the correct number.
            int correct = (result == test->answer);
            if (!correct)
            {
                *ok = 0;
            }

            // Show progress for this case as soon as it finishes.
            if (repeat > 1)
            {
                printf("  [%-8s] case \"%s\" (run %d/%d): %.6f s%s\n",
                       solver->name, test->label, r + 1, repeat, elapsed,
                       correct ? "" : "  [WRONG]");
            }
            else
            {
                printf("  [%-8s] case \"%s\": %.6f s%s\n",
                       solver->name, test->label, elapsed,
                       correct ? "" : "  [WRONG]");
            }
            fflush(stdout);
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
 * difficulty filter, runs the registered solvers while printing per-case
 * progress, then prints a single slowest-to-fastest ranking.
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

    // Collect a result per solver so we can rank them afterwards.
    SolverResult *results = malloc((size_t)solver_count * sizeof(SolverResult));
    if (results == NULL)
    {
        fprintf(stderr, "Out of memory allocating results.\n");
        return 1;
    }

    for (int s = 0; s < solver_count; s++)
    {
        const Solver *solver = solver_registry[s];

        printf("Running solver \"%s\"...\n", solver->name);
        fflush(stdout);

        int ok = 0;
        double average = run_one_solver(solver, repeat, max_difficulty, &ok);

        results[s].name = solver->name;
        results[s].average = average;
        results[s].ok = ok;

        printf("  -> \"%s\" average %.6f s/case (%s)\n\n",
               solver->name, average, ok ? "all correct" : "HAD WRONG ANSWERS");
        fflush(stdout);
    }

    // Sort a ranking from slowest to fastest.
    // A simple insertion sort is plenty for the small solver count.
    for (int i = 1; i < solver_count; i++)
    {
        SolverResult key = results[i];
        int j = i - 1;
        while (j >= 0 && results[j].average < key.average)
        {
            results[j + 1] = results[j];
            j--;
        }
        results[j + 1] = key;
    }

    printf("Ranking (slowest to fastest):\n");
    printf("%-5s %-12s %18s %14s\n",
           "rank", "solver", "avg time/case (s)", "vs. next slower");
    printf("------------------------------------------------------------\n");

    int rank = 0;
    double previous_time = 0.0;
    int have_previous = 0;
    const char *fastest_name = NULL;
    double fastest_time = 0.0;

    for (int i = 0; i < solver_count; i++)
    {
        // Only rank solvers that answered every case correctly.
        if (!results[i].ok)
        {
            continue;
        }
        rank += 1;

        // Percentage improvement over the next-slower ranked solver.
        // The slowest solver has nothing slower to compare against.
        if (!have_previous)
        {
            printf("%-5d %-12s %18.6f %14s\n",
                   rank, results[i].name, results[i].average, "-");
        }
        else
        {
            double improvement = 0.0;
            if (previous_time > 0.0)
            {
                improvement =
                    (previous_time - results[i].average) / previous_time * 100.0;
            }
            printf("%-5d %-12s %18.6f %13.2f%%\n",
                   rank, results[i].name, results[i].average, improvement);
        }

        previous_time = results[i].average;
        have_previous = 1;

        // The list is sorted slowest to fastest, so the last correct entry
        // seen is the fastest correct solver.
        fastest_name = results[i].name;
        fastest_time = results[i].average;
    }

    printf("------------------------------------------------------------\n");
    if (fastest_name != NULL)
    {
        printf("Fastest correct solver: \"%s\" (%.6f s per case)\n",
               fastest_name, fastest_time);
    }
    else
    {
        printf("No solver gave correct answers on all test cases.\n");
    }

    free(results);
    return 0;
}

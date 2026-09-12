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
 *
 * The optional --output="file.csv" flag also writes every per-case timing to a
 * CSV file. That file can be imported into Excel or Google Sheets to draw a
 * graph of the solvers against the test cases.
 */

#include "registry.h"
#include "hash.h"
#include "cases.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * @param csv An open CSV file to append per-case rows to, or NULL to skip it.
 * @return The average time in seconds per case.
 */
static double run_one_solver(const Solver *solver, int repeat,
                             uint64_t max_difficulty, int *ok, FILE *csv)
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

            // Write one CSV row for this run when a CSV file is open. Each row
            // holds the solver, the case, its difficulty, the run number, the
            // time, and whether the answer was correct. One row per run keeps
            // the file easy to pivot or graph in a spreadsheet.
            if (csv != NULL)
            {
                fprintf(csv, "%s,%s,%llu,%d,%.9f,%d\n",
                        solver->name, test->label,
                        (unsigned long long)test->difficulty,
                        r + 1, elapsed, correct ? 1 : 0);
            }

            // Show progress for this case as soon as it finishes.
            if (repeat > 1)
            {
                printf("  [%-8s] case \"%s\" (run %d/%d): %.6fs%s\n",
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
 * @function find_output_path
 * @brief Find the CSV output path from the --output flag.
 *
 * The flag may be written as --output=file.csv or --output="file.csv". Any
 * surrounding double quotes are stripped. The flag may appear in any argument
 * position, so the positional repeat and difficulty arguments still work.
 * @param argc The number of command-line arguments.
 * @param argv The command-line arguments.
 * @return The output path, or NULL when the flag is not present.
 */
static const char *find_output_path(int argc, char **argv)
{
    const char *prefix = "--output=";
    size_t prefix_length = strlen(prefix);

    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], prefix, prefix_length) == 0)
        {
            const char *value = argv[i] + prefix_length;

            // Strip a leading quote so --output="file.csv" also works. The
            // shell usually removes quotes, but this guards against a quoted
            // value that reached the program intact.
            if (value[0] == '"')
            {
                value += 1;
            }
            return value;
        }
    }
    return NULL;
}

/**
 * @function is_flag
 * @brief Tell whether an argument is a named flag rather than a positional one.
 *
 * A flag starts with a leading dash. The positional repeat and difficulty
 * arguments are plain numbers, so this lets main skip flags while it reads
 * them.
 * @param argument The command-line argument to test.
 * @return 1 when the argument is a flag, otherwise 0.
 */
static int is_flag(const char *argument)
{
    return argument[0] == '-';
}

/**
 * @function main
 * @brief Run the benchmark.
 *
 * The program reads optional command-line arguments for repeat count and
 * difficulty filter, plus an optional --output="file.csv" flag. It runs the
 * registered solvers while printing per-case progress, writes per-case rows to
 * the CSV file when the flag is set, then prints a single slowest-to-fastest
 * ranking.
 * @param argc The number of command-line arguments.
 * @param argv The command-line arguments.
 * @return 0 on success.
 */
int main(int argc, char **argv)
{
    const char *output_path = find_output_path(argc, argv);

    // Read the positional repeat and difficulty arguments. Named flags such as
    // --output are skipped so they can sit anywhere on the command line.
    int positional = 0;
    int repeat = 1;
    uint64_t max_difficulty = 0;

    for (int i = 1; i < argc; i++)
    {
        if (is_flag(argv[i]))
        {
            continue;
        }

        positional += 1;
        if (positional == 1)
        {
            repeat = atoi(argv[i]);
            if (repeat < 1)
            {
                repeat = 1;
            }
        }
        else if (positional == 2)
        {
            max_difficulty = strtoull(argv[i], NULL, 10);
        }
    }

    // Open the CSV file up front so a bad path fails before any work is done.
    FILE *csv = NULL;
    if (output_path != NULL)
    {
        csv = fopen(output_path, "w");
        if (csv == NULL)
        {
            fprintf(stderr, "Could not open output file \"%s\".\n",
                    output_path);
            return 1;
        }

        // Write the header row. These names become the column titles when the
        // file is imported into a spreadsheet.
        fprintf(csv, "solver,case,difficulty,run,seconds,correct\n");
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
        if (csv != NULL)
        {
            fclose(csv);
        }
        return 1;
    }

    for (int s = 0; s < solver_count; s++)
    {
        const Solver *solver = solver_registry[s];

        printf("Running solver \"%s\"...\n", solver->name);
        fflush(stdout);

        int ok = 0;
        double average = run_one_solver(solver, repeat, max_difficulty, &ok, csv);

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
           "rank", "solver", "avg time/case (s)", "improvement");
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
        printf("Fastest correct solver: \"%s\" (%.6fs per case)\n",
               fastest_name, fastest_time);
    }
    else
    {
        printf("No solver gave correct answers on all test cases.\n");
    }

    free(results);

    // Close the CSV file and tell the user where the data landed.
    if (csv != NULL)
    {
        fclose(csv);
        printf("\nWrote per-case results to \"%s\".\n", output_path);
    }

    return 0;
}

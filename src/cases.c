/**
 * @file cases.c
 *
 * Preset test cases for the benchmark.
 *
 * Each case has an answer, a start value, a difficulty value, and a label.
 * The benchmark uses these cases to compare solvers.
 */

#include "cases.h"

static const TestCase all_cases[] = {
    // answer,          start,         difficulty,     label

    // --- Group 1: difficulty ramp (answer near the middle) ---
    {500, 0, 1000, "ramp-1k"},
    {5000, 0, 10000, "ramp-10k"},
    {45000, 0, 100000, "ramp-100k"},
    {400000, 0, 1000000, "ramp-1m"},
    {2500000, 0, 5000000, "ramp-5m"},
    {6000000, 0, 10000000, "ramp-10m"},
    {12000000, 0, 25000000, "ramp-25m"},
    {30000000, 0, 50000000, "ramp-50m"},

    // --- Group 2: answer position inside a fixed 10m range ---
    {1, 0, 10000000, "pos-first"},
    {2500000, 0, 10000000, "pos-quarter"},
    {5000000, 0, 10000000, "pos-middle"},
    {7500000, 0, 10000000, "pos-3quarter"},
    {9999999, 0, 10000000, "pos-last"},

    // --- Group 3: non-zero start (a shifted range) ---
    {1000500, 1000000, 1000, "shift-tiny"},
    {1050000, 1000000, 100000, "shift-100k"},
    {5500000, 5000000, 1000000, "shift-1m"},
    {100400000, 100000000, 1000000, "shift-big"},
    {4000000123ULL, 4000000000ULL, 1000000, "shift-huge"},

    // --- Group 4: worst case (answer is the last number of the range) ---
    {99999, 0, 100000, "worst-100k"},
    {999999, 0, 1000000, "worst-1m"},
    {9999999, 0, 10000000, "worst-10m"},
    {19999999, 0, 20000000, "worst-20m"},

    // --- Group 5: a few large, mixed cases for a stress comparison ---
    {33333333, 0, 40000000, "stress-40m"},
    {55000000, 10000000, 60000000, "stress-60m"},
    {88888888, 0, 100000000, "stress-100m"},
};

// These two symbols give the benchmark a view of the list.
const TestCase *const preset_cases = all_cases;
const int preset_count = (int)(sizeof(all_cases) / sizeof(all_cases[0]));

# Reverse-Hashing Benchmark

C benchmark for testing SHA-256 brute-force search algorithms for the DTU 02159 OS Challenge.

The project is for testing algorithms. It isn't the challenge server.

## Build and run

```bash
make
make run
```

Run the benchmark directly:

```bash
./build/benchmark
```

Repeat each test:

```bash
./build/benchmark 5
```

Only run small tests:

```bash
./build/benchmark 3 1000000
```

Arguments:

```text
benchmark [repeats] [max_difficulty] [--solver="name"] [--output="file.csv"]
```

* `repeats`: Number of runs per test. Default: `1`.
* `max_difficulty`: Maximum range size. `0` runs all tests.
* `--solver="name"`: Only run the named solver(s). Repeatable and accepts a
  comma-separated list. Runs all solvers when omitted.
* `--output="file.csv"`: Write every per-case timing to a CSV file.

## Run specific solvers

Add the `--solver` flag to run only the solvers you name:

```bash
./build/benchmark --solver="dynamic"
```

Run several by repeating the flag or with a comma-separated list:

```bash
./build/benchmark --solver="threaded,dynamic"
```

The flag can sit anywhere on the command line. Without it, the benchmark runs
every solver.

## Export results to CSV

Add the `--output` flag to write the results to a CSV file:

```bash
./build/benchmark 5 --output="results.csv"
```

The flag can sit anywhere on the command line. The file has one row per
solver and test case. With repeats, the `seconds` column is the average over
the runs, and the `runs` column says how many runs the average covers:

```text
solver,case,difficulty,runs,seconds,correct
linear,ramp-1k,1000,1,0.000336000,1
threaded,ramp-1k,1000,1,0.000196000,1
```

Import the file into Excel or Google Sheets to graph the solvers against the
test cases. The `difficulty` column works well as the x-axis, `seconds` as the
y-axis, and `solver` as the series.

Clean the build:

```bash
make clean
```

## Add an algorithm

Create a file in:

```text
src/solvers/
```

Implement:

```c
uint64_t my_solver(
    const uint8_t *target,
    uint64_t start,
    uint64_t end
);
```

Add the solver:

```c
const Solver solver_my_solver = {
    "my_solver",
    my_solver
};
```

Register it in `src/registry.c`:

```c
extern const Solver solver_my_solver;
```

and add it to `all_solvers`.

Add the new `.c` file to `SOURCES` in the `Makefile`.

Then run:

```bash
make run
```

## Test cases

Test cases are in:

```text
src/cases.c
```

Each case has:

* An answer
* A start value
* A range size

The answer must satisfy:

```text
start <= answer < start + difficulty
```

The benchmark creates the SHA-256 hash from the answer. It then checks if each solver finds the answer.

## Project structure

```text
src/
├── benchmark.c       Benchmark
├── cases.c/h         Test cases
├── hash.c/h          SHA-256 wrapper
├── registry.c/h      Solver list
├── solver.h          Solver interface
└── solvers/          Solver implementations
    ├── dynamic.c
    ├── linear.c
    └── threaded.c

vendor/
└── lonesha256.h      Provided SHA-256 implementation
```

Do not change `vendor/lonesha256.h`.

## Rules

* Input is a `uint64_t`.
* Input uses little-endian byte order.
* Use `lonesha256`.
* `-O3` is allowed.
* `libc` and `pthread` are allowed.
* GPU use isn't allowed.
* Pre-computed rainbow tables are not allowed.

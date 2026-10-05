# Threaded Prime Number Search

Finds every prime from 1 to **y** using **x** threads (`std::thread`). `config.txt` sets x and y, and chooses one of two printing modes and one of two ways to divide the work, for four combinations in total:

| `print` | `division` | When primes are printed | How the work is divided |
|---|---|---|---|
| `immediate` | `straight` | Immediately, with thread id and timestamp | The range 1–y is split into x contiguous blocks |
| `immediate` | `divisibility` | Immediately, with thread id and timestamp | Numbers are tested one at a time; the divisors of each number are split across x threads |
| `end` | `straight` | After all threads are joined | The range 1–y is split into x contiguous blocks |
| `end` | `divisibility` | After all threads are joined | Numbers are tested one at a time; the divisors of each number are split across x threads |

Files:

- `main.cpp`: reads the config, prints the plan, runs the chosen search and prints the summary.
- `straight_division.h`: the straight-division search.
- `divisibility_testing.h`: the divisibility-testing search.
- `prime_output.h`: prints each prime immediately, or records it for printing after the join, depending on `print`.
- `run_threads.h`: creates the x threads, starts them together and joins them, and reports cleanly if the system can't create them all.
- `config.h`, `primes.h`, `timing.h`: reading the config file, primality test and range splitting, timestamps.
- `config.txt`: the settings.

## Configuration

```ini
# threads (at least 1, no upper limit)
x=4
# range: search for primes from 1 up to and including y (1 - 1000000000)
y=1000
# when primes are printed: immediate (as each thread finds them) or end (after all threads finish)
print=immediate
# how the work is split: straight (range 1 - y cut into x blocks) or divisibility (each number's divisors split across threads)
division=straight
```

The program reads `config.txt` from the current folder. To use a different file, pass its path as the first argument: `prime_search.exe other_config.txt`.

The file is checked before any thread starts:

- Each of the four settings must appear exactly once, in any order. Setting names are lowercase. The `print` and `division` values are not case-sensitive.
- `x` and `y` are written with digits only: no signs, commas, decimals or exponents.
- `x` must be at least 1 and has no upper limit. The program tries to create as many threads as you ask for (see [Large thread counts](#large-thread-counts)).
- `y` must be from 1 to 1,000,000,000. The cap stops a typo from starting a run that would take days or, with `print=end`, run out of memory.
- The file must be saved as UTF-8 (with or without a byte order mark) or ANSI. UTF-16 files, which Windows PowerShell's `>` creates, are rejected with a message saying so.

If anything is wrong, the program names the file, the line and the problem, then exits with code 1. For example:

```
Config error: config.txt, line 3: x is already set on line 1
```

## Building

You need a C++20 compiler, because the program uses `<barrier>`. Any of these will work:

- **g++ 11 or newer**. On Windows, use MinGW-w64 with POSIX threads, from [MSYS2](https://www.msys2.org/) (UCRT64) or [WinLibs](https://winlibs.com/). The code was tested with WinLibs g++ 16.2.
- **Visual Studio 2022** (MSVC).
- **A recent Clang** with a standard library that provides `<barrier>`.

**Windows with g++:** from this folder, run

```bat
build.bat
```

which runs

```bat
g++ -std=c++20 -O2 -Wall -Wextra -pthread -static main.cpp -o prime_search.exe
```

`-static` lets the `.exe` run without MinGW's DLLs on the PATH.

**MSVC** (in a *Developer Command Prompt for VS 2022*):

```bat
cl /std:c++20 /O2 /EHsc /W4 main.cpp /Fe:prime_search.exe
```

**Linux / macOS:**

```sh
g++ -std=c++20 -O2 -Wall -Wextra -pthread main.cpp -o prime_search
```

The four modes are chosen at run time, so one build covers all of them. Change `config.txt` and run again; there is no need to rebuild.

## Running

```bat
prime_search.exe
```

Each run prints the following, in order:

1. The settings and how the work is divided. With `division=straight`, each thread's block is listed. With `division=divisibility`, the divisors of y are split as an example.
2. `Start time`: the date and time, to the microsecond.
3. The primes, one per line, formatted as `[HH:MM:SS.uuuuuu] [Thread k] prime`. With `print=end`, the timestamp is when the prime was *found*, not when it was printed.
4. `End time`, the elapsed time and the number of primes found. With `print=end`, the elapsed time is split into search time and printing time.
5. With `division=straight`, how many primes each thread found and when each thread finished. The main thread's `join()` waits for the slowest of them.

With large limits, writing to the console takes more time than the search itself. To time the search, redirect the output to a file (`prime_search.exe > out.txt`) and read the summary at the end of the file.

### Large thread counts

All x threads are created first and held at a start gate. Once every one of them exists, they are released together. If the operating system can't create all x, the threads that were created exit without searching, and the program exits with code 1 and a message like:

```
Error: the system could only create 14177 of the 200000 threads asked for (Resource temporarily unavailable)
```

How many threads the system can create depends on its free memory. If x is too large even to set up the per-thread bookkeeping, the program reports that it ran out of memory instead.

With more than 64 threads, the per-thread lists show only the first 10 and last 10 threads. The summary always names the slowest thread with `division=straight`.

## Changing config.txt

**Straight division (`division=straight`).** `splitRange` cuts 1–y into x contiguous blocks. For y = 1000 and x = 4, the blocks are 1–250, 251–500, 501–750 and 751–1000. When y doesn't divide evenly, the first blocks get one extra number each. Each thread tests every number in its block independently using trial division (`isPrime`). The threads never wait for each other until the join. Numbers in higher blocks cost more to test, so the blocks don't take the same time to finish.

**Divisibility testing (`division=divisibility`).** The x threads are created once and live for the whole run. For each number n (2, 3, …, y), the candidate divisors 2 to ⌊√n⌋ are split into x blocks, one per thread. A thread stops early if another thread has already found a divisor; they share an `std::atomic<bool>` for this. A `std::barrier` makes every thread finish n before any thread starts n + 1. The barrier's completion step runs on the last thread to arrive. It decides whether n is prime, prints or records it under that thread's id, and moves on to n + 1. This means the program synchronizes all threads once per number, y − 1 times in total. Each of those rounds does very little work, so this mode is much slower than straight division.

**Immediate printing (`print=immediate`).** Each output line is built in full, then written while holding a mutex. Lines from different threads interleave with each other, but a line is never split by another thread's output. The timestamp is taken before the thread waits for the lock. As a result, with `division=straight` the lines can appear slightly out of timestamp order, which shows interleaving at work.

**Printing at the end (`print=end`).** While searching, each thread records the primes it finds in its own list, so recording needs no locks. After every thread has been joined, the main thread merges the lists and prints the primes in ascending order.

All four combinations use the same primality test: trial division by 2 to ⌊√n⌋. Their timings can therefore be compared directly. Any difference comes from how the work is divided and when the output is printed.

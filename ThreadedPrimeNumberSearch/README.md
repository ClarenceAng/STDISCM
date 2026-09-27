# Threaded Prime Number Search

Finds every prime from 1 to **y** using **x** threads (`std::thread`). The values of x and y are read from `config.txt`. There are four variants, one per folder:

| Folder | When primes are printed | How the work is divided |
|---|---|---|
| `Variant1_Immediate_StraightDivision` | Immediately, with thread id and timestamp | The range 1–y is split into x contiguous blocks |
| `Variant2_Immediate_DivisibilityTesting` | Immediately, with thread id and timestamp | Numbers are tested one at a time; the divisors of each number are split across x threads |
| `Variant3_PrintAtEnd_StraightDivision` | After all threads are joined | The range 1–y is split into x contiguous blocks |
| `Variant4_PrintAtEnd_DivisibilityTesting` | After all threads are joined | Numbers are tested one at a time; the divisors of each number are split across x threads |

Every folder builds on its own and contains:

- `main.cpp`: the variant's threading and printing logic.
- `config.h`, `primes.h`, `timing.h`: shared helpers (reading the config file, primality test and range splitting, timestamps). These files are identical in all four folders.
- `config.txt`: the settings for x and y.

## Configuration

```ini
# x: number of threads to create (1 - 1024)
x = 4

# y: search for primes from 1 up to and including this number
y = 1000
```

The program reads `config.txt` from the current folder. To use a different file, pass its path as the first argument: `prime_search.exe other_config.txt`. If the file has an invalid value, a misspelled key or a missing setting, the program prints the line at fault and exits with code 1.

## Building

You need a C++20 compiler, because the program uses `<barrier>`. Any of these will work:

- **g++ 11 or newer**. On Windows, use MinGW-w64 with POSIX threads, from [MSYS2](https://www.msys2.org/) (UCRT64) or [WinLibs](https://winlibs.com/). The code was tested with WinLibs g++ 16.2.
- **Visual Studio 2022** (MSVC).
- **A recent Clang** with a standard library that provides `<barrier>`.

**Windows, all four variants with g++:** from this folder, run

```bat
build_all.bat
```

**One variant with g++ (Windows):**

```bat
cd Variant1_Immediate_StraightDivision
g++ -std=c++20 -O2 -Wall -Wextra -pthread -static main.cpp -o prime_search.exe
```

`-static` lets the `.exe` run without MinGW's DLLs on the PATH.

**One variant with MSVC** (in a *Developer Command Prompt for VS 2022*):

```bat
cd Variant1_Immediate_StraightDivision
cl /std:c++20 /O2 /EHsc /W4 main.cpp /Fe:prime_search.exe
```

**Linux / macOS:**

```sh
cd Variant1_Immediate_StraightDivision
g++ -std=c++20 -O2 -Wall -Wextra -pthread main.cpp -o prime_search
```

## Running

```bat
cd Variant1_Immediate_StraightDivision
prime_search.exe
```

Each run prints the following, in order:

1. How the work is divided. The straight-division variants list each thread's block. The divisibility variants show how the divisors of y are split, as an example.
2. `Start time`: the date and time, to the microsecond.
3. The primes, one per line, formatted as `[HH:MM:SS.uuuuuu] [Thread k] prime`. In the print-at-end variants, the timestamp is when the prime was *found*, not when it was printed.
4. `End time`, the elapsed time and the number of primes found. The print-at-end variants split the elapsed time into search time and printing time.
5. The straight-division variants also show how many primes each thread found and when each thread finished. The main thread's `join()` waits for the slowest of them.

With large limits, writing to the console takes more time than the search itself. To time the search, redirect the output to a file (`prime_search.exe > out.txt`) and read the summary at the end of the file.

## Implementation notes

**Straight division (variants 1 and 3).** `splitRange` cuts 1–y into x contiguous blocks. For y = 1000 and x = 4, the blocks are 1–250, 251–500, 501–750 and 751–1000. When y doesn't divide evenly, the first blocks get one extra number each. Each thread tests every number in its block independently using trial division (`isPrime`). Numbers in higher blocks cost more to test, so the blocks don't take the same time to finish.

**Divisibility testing (variants 2 and 4).** The x threads are created once and live for the whole run. For each number n (2, 3, …, y), the candidate divisors 2 to ⌊√n⌋ are split into x blocks, one per thread. A thread stops early if another thread has already found a divisor; they share an `std::atomic<bool>` for this. A `std::barrier` makes every thread finish n before any thread starts n + 1. The barrier's completion step runs on the last thread to arrive. It decides whether n is prime, prints or records it under that thread's id, and moves on to n + 1. This means the program synchronizes all threads once per number, y − 1 times in total.

**Immediate printing (variants 1 and 2).** Each output line is built in full, then written while holding a mutex. Lines from different threads interleave with each other, but a line is never split by another thread's output. The timestamp is taken before the thread waits for the lock. As a result, variant 1's lines can appear slightly out of timestamp order, which shows interleaving at work.

**Printing at the end (variants 3 and 4).** While searching, each thread records the primes it finds in its own list, so the threads share nothing and need no locks. After every thread has been joined, the main thread prints all the lists. The primes come out in ascending order.

All four variants use the same primality test: trial division by 2 to ⌊√n⌋. Their timings can therefore be compared directly. Any difference comes from how the work is divided and when the output is printed.

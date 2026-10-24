# Group Synchronization (LFG Dungeon Queue)

Simulates a dungeon queue in an MMORPG. Players are grouped into parties of **1 tank, 1 healer and 3 DPS**. At most **n** dungeon instances can run at the same time, and each party clears its instance in a random time from **t1** to **t2** seconds. A `std::counting_semaphore` with n permits limits how many instances are active at once, and a mutex protects the shared instance status and the console output. Each party goes to the empty instance that has served the fewest parties so far.

Files:

- `main.cpp`: forms the parties, sends them into the instances, prints each status change and the final summary.
- `config.h`: reads and checks `config.txt`.
- `config.txt`: the settings.
- `build.bat`: builds `lfg.exe` on Windows with g++.
- `Makefile`: builds `lfg` on Linux / macOS.

## Building

You need a C++20 compiler, because the program uses `<semaphore>`. Any of these will work:

- **g++ 11 or newer**. On Windows, use MinGW-w64 with POSIX threads, from [MSYS2](https://www.msys2.org/) (UCRT64) or [WinLibs](https://winlibs.com/). The code was tested with WinLibs g++ 16.2.
- **Visual Studio 2022** (MSVC).
- **A recent Clang** with a standard library that provides `<semaphore>`.

There is only one source file, `main.cpp`, which includes `config.h`.

**Windows with g++:** from this folder, run

```bat
build.bat
```

which checks that `g++` is on the PATH, then runs

```bat
g++ -std=c++20 -O2 -Wall -Wextra -pthread -static main.cpp -o lfg.exe
```

`-static` lets the `.exe` run without MinGW's DLLs on the PATH.

**MSVC** (in a *Developer Command Prompt for VS 2022*):

```bat
cl /std:c++20 /O2 /EHsc /W4 main.cpp /Fe:lfg.exe
```

**Linux / macOS:**

```sh
make
```

which runs

```sh
g++ -std=c++20 -O2 -Wall -Wextra -pedantic -pthread main.cpp -o lfg
```

`make` uses your system's default C++ compiler (usually `g++`). To build with Clang, run `make CXX=clang++`. `make clean` removes the binary.

All the settings are read at run time, so you don't need to rebuild after changing `config.txt`.

## Running

```bat
lfg.exe
```

(`./lfg` on Linux / macOS.)

The program reads `config.txt` from the current folder. To use a different file, pass its path as the only argument: `lfg.exe other_config.txt`.

Each run prints the following, in order:

1. The settings, the number of parties formed and the players left over.
2. The status of every instance (`empty` or `active`).
3. One line each time a party enters or clears an instance, with the time since the start and the clear time. The status of every instance is printed after each line.
4. A summary with the parties served and the total time served by each instance, the totals, the players left over and the elapsed time.

Clear times are real `sleep`s, so a run takes roughly (parties × average clear time) / n seconds. With the default `config.txt` (48 parties, n = 10, 5–10 s), that is about 40 seconds.

The exit code is 0 on success and 1 if the config is invalid or a party could not be started or finished (for example, if the system can't create another thread).

## Configuration

```ini
# max concurrent instances
n=10
# tanks, healers and DPS in the queue
t=54
h=52
d=146
# clear time range in seconds (t1 <= t2 <= 15)
t1=5
t2=10
```

| Setting | Meaning | Allowed values |
|---|---|---|
| `n` | Maximum number of instances running at the same time | 1 – 2,147,483,647 |
| `t` | Tank players in the queue | 0 – 2,147,483,647 |
| `h` | Healer players in the queue | 0 – 2,147,483,647 |
| `d` | DPS players in the queue | 0 – 2,147,483,647 |
| `t1` | Shortest clear time, in seconds | 0 – 15, and at most `t2` |
| `t2` | Longest clear time, in seconds | 0 – 15 |

The number of parties is `min(t, h, d / 3)`. Players who can't fill a party are reported as left over.

The file is checked before any party starts:

- Each of the six settings must appear exactly once, in any order. Setting names are lowercase. Spaces around `=` are fine, and `#` starts a comment.
- Values are written with digits only: no signs, commas, decimals or exponents.
- The file must be saved as UTF-8 (with or without a byte order mark) or ANSI. UTF-16 files, which Windows PowerShell's `>` creates, are rejected with a message saying so.
- A very large `n` is accepted by the checks, but the program stops with an error if there isn't enough memory to track that many instances.

If anything is wrong, the program names the file, the line and the problem, then exits with code 1. For example:

```
Config error: config.txt, line 8: t1 = 5 is more than t2 = 4 (line 9)
```

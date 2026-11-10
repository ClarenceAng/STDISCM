@echo off
rem usage: run.bat [video or folder] ...     any paths given are uploaded first, then it keeps listening for more
setlocal

rem no cd, so relative paths still mean relative to where this was run from
set "ROOT=%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
    echo g++ was not found on the PATH. It is needed to build the command line program.
    exit /b 1
)

where docker >nul 2>nul
if errorlevel 1 (
    echo docker was not found on the PATH.
    exit /b 1
)

g++ -std=c++17 -O2 -Wall -Wextra -pthread -static "%ROOT%tools\upload_cli.cpp" -o "%ROOT%tools\upload_cli.exe"
if errorlevel 1 (
    echo Could not build the command line program, the compiler's message is above.
    echo If it says permission denied, the program is already running in another window.
    exit /b 1
)

"%ROOT%tools\upload_cli.exe" "%ROOT%config.txt" "%ROOT%docker-compose.yml" %*
exit /b %errorlevel%

@echo off
rem Builds prime_search.exe with g++ (MinGW-w64).
setlocal

where g++ >nul 2>nul
if errorlevel 1 (
    echo g++ was not found on PATH. See README.md for how to install a compiler or build with Visual Studio.
    exit /b 1
)

cd /d "%~dp0"
g++ -std=c++20 -O2 -Wall -Wextra -pthread -static main.cpp -o prime_search.exe
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

echo Built prime_search.exe. Pick the print and division modes in config.txt, then run prime_search.exe
exit /b 0

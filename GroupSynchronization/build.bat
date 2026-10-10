@echo off
rem Builds lfg.exe with g++ (MinGW-w64).
setlocal

where g++ >nul 2>nul
if errorlevel 1 (
    echo g++ was not found on PATH. Install MinGW-w64 g++ 11 or newer, or build with Visual Studio.
    exit /b 1
)

cd /d "%~dp0"
g++ -std=c++20 -O2 -Wall -Wextra -pthread -static main.cpp -o lfg.exe
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

echo Built lfg.exe. Set the inputs in config.txt, then run lfg.exe
exit /b 0

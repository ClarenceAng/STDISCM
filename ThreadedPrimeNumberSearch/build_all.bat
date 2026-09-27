@echo off
rem Builds all four variants with g++ (MinGW-w64). Each variant folder gets its own prime_search.exe.
setlocal

where g++ >nul 2>nul
if errorlevel 1 (
    echo g++ was not found on PATH. See README.md for how to install a compiler or build with Visual Studio.
    exit /b 1
)

cd /d "%~dp0"
for /d %%V in (Variant*) do (
    echo Building %%V
    pushd "%%V"
    g++ -std=c++20 -O2 -Wall -Wextra -pthread -static main.cpp -o prime_search.exe
    if errorlevel 1 (
        popd
        echo Build failed in %%V
        exit /b 1
    )
    popd
)

echo.
echo Done. To run a variant: cd into its folder and run prime_search.exe

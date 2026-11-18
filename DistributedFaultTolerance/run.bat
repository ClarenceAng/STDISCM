@echo off
rem Builds every image, starts every node in the background and waits until the site answers.
setlocal
cd /d "%~dp0"

where docker >nul 2>nul
if errorlevel 1 (
    echo docker was not found on the PATH. Install Docker Desktop and start it.
    exit /b 1
)
docker info >nul 2>nul
if errorlevel 1 (
    echo Docker is installed but not running. Start Docker Desktop and try again.
    exit /b 1
)
docker compose version >nul 2>nul
if errorlevel 1 (
    echo "docker compose" is not available. Docker Compose v2 is needed.
    exit /b 1
)

docker compose up --build -d
if errorlevel 1 (
    echo Starting the containers failed. The messages above say why.
    exit /b 1
)

if "%VIEW_PORT%"=="" set "VIEW_PORT=8080"
echo Waiting for the site on http://localhost:%VIEW_PORT% ...
for /l %%i in (1,1,60) do (
    curl -s -o nul http://localhost:%VIEW_PORT%/login && goto ready
    ping -n 3 127.0.0.1 >nul
)
echo The site did not answer within 2 minutes. See: docker compose logs view
exit /b 1

:ready
rem The first start also creates and copies the database, which takes a little longer.
echo Waiting for the database nodes ...
for /l %%i in (1,1,90) do (
    curl -s http://localhost:%VIEW_PORT%/status | findstr /c:"Commits wait for the standby" >nul && goto dbready
    ping -n 3 127.0.0.1 >nul
)
echo The database nodes were not ready within 3 minutes. The status page says why:
echo   http://localhost:%VIEW_PORT%/status   or run: docker compose logs db1 db2 dbmonitor
exit /b 1

:dbready
echo.
echo Ready: http://localhost:%VIEW_PORT%   (all passwords are "password")
echo   student01..student60, msantos / jreyes / acruz / mtan (faculty), admin
echo.
echo Stop a node:       docker compose stop course
echo Start it again:    docker compose start course
echo Stop everything:   docker compose down      (add -v to also delete the database)
exit /b 0

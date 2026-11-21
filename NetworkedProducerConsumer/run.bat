@echo off
rem usage: run.bat     then put videos, or folders of videos, in the videos folder next to this file, and press Ctrl+C to stop
setlocal

rem the folder this script is in, with a trailing backslash
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

where mkcert >nul 2>nul
if errorlevel 1 (
    echo mkcert was not found on the PATH. It makes the certificates for the HTTPS GUI and the producers' encrypted connections.
    echo Install it with   winget install FiloSottile.mkcert   and then open a new terminal so the PATH is updated.
    exit /b 1
)

rem ---- certificates -----------------------------------------------------------------------------
rem mkcert keeps one certificate authority per user (mkcert -CAROOT says where). certs\ gets a copy
rem of its certificate, ca.pem, which the producers trust, and a certificate signed by it for the
rem consumer. The authority's private key never leaves mkcert's folder.
set "CERTS=%ROOT%certs"
set "MKCERT_LOG=%CERTS%\mkcert.log"
if not exist "%CERTS%\consumer" mkdir "%CERTS%\consumer"
if not exist "%CERTS%\consumer" (
    echo Could not create the folder %CERTS%\consumer for the certificates.
    exit /b 1
)

rem Puts the authority into the Windows trust store, so browsers trust the GUI's certificate. The first
rem time Windows asks to confirm; after that it does nothing. Only the system store: Chrome and Edge
rem use it, and mkcert's Java step would fail without admin rights when JAVA_HOME is set.
set "TRUST_STORES=system"
mkcert -install >"%MKCERT_LOG%" 2>&1
if errorlevel 1 (
    type "%MKCERT_LOG%"
    echo mkcert could not add its certificate authority to the Windows trust store, its message is above.
    echo If Windows asked whether to install the certificate, the answer has to be Yes.
    exit /b 1
)

set "CA_FOLDER="
for /f "usebackq delims=" %%d in (`mkcert -CAROOT`) do set "CA_FOLDER=%%d"
if not defined CA_FOLDER (
    echo mkcert did not say where its certificate authority is ^(mkcert -CAROOT^).
    exit /b 1
)
rem An empty folder named ca.pem is what Docker leaves when the containers were started before the
rem certificates existed (it mounts a missing file as a new folder). It would stop the copy below.
rem The attributes tell a folder from a file: they start with d. The usual test, if exist "ca.pem\",
rem is also true for a plain file on some drives, this project's D: among them.
set "CA_ATTRIBUTES=-"
for %%f in ("%CERTS%\ca.pem") do set "CA_ATTRIBUTES=-%%~af"
if "%CA_ATTRIBUTES:~1,1%"=="d" (
    rmdir "%CERTS%\ca.pem" || (
        echo %CERTS%\ca.pem is a folder with something in it, so mkcert's certificate authority can't be copied there.
        exit /b 1
    )
)
copy /y "%CA_FOLDER%\rootCA.pem" "%CERTS%\ca.pem" >nul
if errorlevel 1 (
    echo Could not copy mkcert's certificate authority from %CA_FOLDER%\rootCA.pem to %CERTS%\ca.pem.
    exit /b 1
)

rem "consumer" is the name the producers connect to, localhost and 127.0.0.1 are what the browser uses.
rem Made again on every start, so it always comes from the authority this computer trusts now.
mkcert -cert-file "%CERTS%\consumer\cert.pem" -key-file "%CERTS%\consumer\key.pem" consumer localhost 127.0.0.1 ::1 >"%MKCERT_LOG%" 2>&1
if errorlevel 1 (
    type "%MKCERT_LOG%"
    echo mkcert could not make the consumer's certificate, its message is above.
    exit /b 1
)
del "%MKCERT_LOG%" >nul 2>nul

rem ---- the command line program ------------------------------------------------------------------
g++ -std=c++17 -O2 -Wall -Wextra -pthread -static "%ROOT%tools\upload_cli.cpp" -o "%ROOT%tools\upload_cli.exe"
if errorlevel 1 (
    echo Could not build the command line program, the compiler's message is above.
    echo If it says permission denied, the program is already running in another window.
    exit /b 1
)

"%ROOT%tools\upload_cli.exe" "%ROOT%config.txt" "%ROOT%docker-compose.yml"
exit /b %errorlevel%

@echo off
rem Makes the TLS certificates, builds every image, starts every node in the background and waits until the site answers.
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
where mkcert >nul 2>nul
if errorlevel 1 (
    echo mkcert was not found on the PATH. It makes the certificates for HTTPS and for the encrypted gRPC links between nodes.
    echo Install it with   winget install FiloSottile.mkcert   and then open a new terminal so the PATH is updated.
    exit /b 1
)

rem ---- certificates -----------------------------------------------------------------------------
rem mkcert keeps one certificate authority per user (mkcert -CAROOT says where). certs\ gets a copy
rem of its certificate, ca.pem, which the view trusts, and one certificate signed by it for each node,
rem for the name the view dials. The authority's private key never leaves mkcert's folder.
set "CERTS=%CD%\certs"
set "MKCERT_LOG=%CERTS%\mkcert.log"
if not exist "%CERTS%" mkdir "%CERTS%"
if not exist "%CERTS%" (
    echo Could not create the folder %CERTS% for the certificates.
    exit /b 1
)

rem Puts the authority into the Windows trust store, so browsers trust the site's certificate. The
rem first time Windows asks to confirm; after that it does nothing. Only the system store: Chrome and
rem Edge use it, and mkcert's Java step would fail without admin rights when JAVA_HOME is set.
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
rem Running nodes only read their certificates when they start. Ones from the same authority are
rem still good, but if the authority changed (or ca.pem is a stray folder, see below), every node
rem has to start again to load the new ones.
set "RECREATE="
if exist "%CERTS%\ca.pem" (
    fc /b "%CA_FOLDER%\rootCA.pem" "%CERTS%\ca.pem" >nul 2>nul
    if errorlevel 1 set "RECREATE=--force-recreate"
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

rem Each backend node's certificate is for its compose service name only. Made again on every
rem start, so they always come from the authority this computer trusts now.
for %%n in (auth course enrollment grade admin dbmonitor) do (
    if not exist "%CERTS%\%%n" mkdir "%CERTS%\%%n"
    mkcert -cert-file "%CERTS%\%%n\cert.pem" -key-file "%CERTS%\%%n\key.pem" %%n >"%MKCERT_LOG%" 2>&1
    if errorlevel 1 (
        type "%MKCERT_LOG%"
        echo mkcert could not make the certificate for the %%n node, its message is above.
        exit /b 1
    )
)
rem the view's is for what the browser opens
if not exist "%CERTS%\view" mkdir "%CERTS%\view"
mkcert -cert-file "%CERTS%\view\cert.pem" -key-file "%CERTS%\view\key.pem" localhost 127.0.0.1 ::1 view >"%MKCERT_LOG%" 2>&1
if errorlevel 1 (
    type "%MKCERT_LOG%"
    echo mkcert could not make the certificate for the view node, its message is above.
    exit /b 1
)
del "%MKCERT_LOG%" >nul 2>nul

rem ---- containers -------------------------------------------------------------------------------
if defined RECREATE echo certs\ca.pem was not mkcert's current certificate authority, so every node is started again to load the new certificates.
docker compose up --build -d %RECREATE%
if errorlevel 1 (
    echo Starting the containers failed. The messages above say why.
    exit /b 1
)

rem curl checks the site's certificate against mkcert's authority, the same way a browser does
rem --ssl-revoke-best-effort: Windows' curl also asks whether the certificate was revoked, and
rem mkcert's certificates name nowhere to ask, so that check would always fail.
if "%VIEW_PORT%"=="" set "VIEW_PORT=8080"
set "SITE=https://localhost:%VIEW_PORT%"
set "CURL=curl -s --cacert "%CERTS%\ca.pem" --ssl-revoke-best-effort"
echo Waiting for the site on %SITE% ...
for /l %%i in (1,1,60) do (
    %CURL% -o nul %SITE%/login && goto ready
    rem 60: the site answered but its certificate was refused, which waiting won't fix
    if errorlevel 60 if not errorlevel 61 goto certrefused
    ping -n 3 127.0.0.1 >nul
)
echo The site did not answer within 2 minutes. curl's last reason:
%CURL% -S -o nul %SITE%/login
echo See: docker compose logs view
exit /b 1

:certrefused
echo The site answered, but its certificate was refused. curl's reason:
%CURL% -S -o nul %SITE%/login
echo Run run.bat again to make new certificates. If this repeats, see: docker compose logs view
exit /b 1

:ready
rem The first start also creates and copies the database, which takes a little longer.
echo Waiting for the database nodes ...
for /l %%i in (1,1,90) do (
    %CURL% %SITE%/status | findstr /c:"Commits wait for the standby" >nul && goto dbready
    ping -n 3 127.0.0.1 >nul
)
echo The database nodes were not ready within 3 minutes. The status page says why:
echo   %SITE%/status   or run: docker compose logs db1 db2 dbmonitor
exit /b 1

:dbready
echo.
echo Ready: %SITE%   (all passwords are "password")
echo   student01..student60, msantos / jreyes / acruz / mtan (faculty), admin
echo.
echo Stop a node:       docker compose stop course
echo Start it again:    docker compose start course
echo Stop everything:   docker compose down      (add -v to also delete the database)
exit /b 0

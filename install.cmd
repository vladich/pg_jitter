@echo off
setlocal enabledelayedexpansion
REM install.cmd -- Install pg_jitter backends (Windows / MSVC)
REM
REM Usage:
REM   install.cmd [options] [sljit^|asmjit^|mir^|all]
REM
REM Options:
REM   --pg-config PATH   Path to pg_config.exe
REM   --pgdata DIR       PostgreSQL data directory
REM   --restart          Restart PostgreSQL after install (off by default)
REM
REM Examples:
REM   install.cmd                                           Install all backends
REM   install.cmd sljit                                     Install sljit only
REM   install.cmd --pg-config C:\pg18\bin\pg_config all     Custom PG install
REM   install.cmd --restart sljit                           Install and restart
REM
REM pg_config resolution (first match wins):
REM   1. --pg-config argument
REM   2. PG_CONFIG environment variable
REM   3. pg_config from PATH
REM
REM pgdata resolution (first match wins):
REM   1. --pgdata argument
REM   2. PGDATA environment variable
REM   3. Query running PostgreSQL via psql

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---- Parse named arguments ----
set "PG_CONFIG_ARG="
set "PGDATA_ARG="
set "DO_RESTART=0"

:parse_args
if "%~1"=="" goto done_parse
if /i "%~1"=="--pg-config" (
    if "%~2"=="" (
        echo ERROR: --pg-config requires a path argument
        exit /b 1
    )
    set "PG_CONFIG_ARG=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--pgdata" (
    if "%~2"=="" (
        echo ERROR: --pgdata requires a path argument
        exit /b 1
    )
    set "PGDATA_ARG=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--restart" (
    set "DO_RESTART=1"
    shift
    goto parse_args
)
goto done_parse

:done_parse

REM ---- Resolve pg_config ----
if defined PG_CONFIG_ARG (
    set "PG_CONFIG=%PG_CONFIG_ARG%"
) else if not defined PG_CONFIG (
    where pg_config >nul 2>&1
    if !errorlevel! equ 0 (
        set "PG_CONFIG=pg_config"
    ) else (
        echo ERROR: pg_config not found.
        echo   Use: install.cmd --pg-config C:\path\to\pg_config.exe
        exit /b 1
    )
)

REM Verify pg_config works
"%PG_CONFIG%" --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: pg_config failed: %PG_CONFIG%
    exit /b 1
)

REM ---- Get PG paths ----
for /f "usebackq delims=" %%p in (`"%PG_CONFIG%" --bindir`) do set "PGBIN=%%p"
for /f "usebackq delims=" %%p in (`"%PG_CONFIG%" --pkglibdir`) do set "PKGLIBDIR=%%p"

REM ---- Determine PG version ----
for /f "tokens=2 delims= " %%v in ('"%PG_CONFIG%" --version') do set "PG_FULL_VER=%%v"
for /f "tokens=1 delims=." %%m in ("%PG_FULL_VER%") do set "PG_MAJOR=%%m"

REM ---- Resolve data directory (only needed for --restart) ----
set "PG_DATA="
if "%DO_RESTART%"=="1" (
    if defined PGDATA_ARG (
        set "PG_DATA=%PGDATA_ARG%"
    ) else if defined PGDATA (
        set "PG_DATA=%PGDATA%"
    ) else (
        set "_port=5432"
        if defined PGPORT set "_port=%PGPORT%"
        for /f "usebackq delims=" %%d in (`"%PGBIN%\psql" -p !_port! -d postgres -t -A -c "SHOW data_directory;" 2^>nul`) do set "PG_DATA=%%d"
    )
    if not defined PG_DATA (
        echo WARNING: --restart requested but data directory not found.
        echo   Set PGDATA or use --pgdata DIR, or ensure PostgreSQL is running.
        set "DO_RESTART=0"
    ) else if not exist "!PG_DATA!" (
        echo WARNING: --restart requested but data directory not found: !PG_DATA!
        set "DO_RESTART=0"
    )
)

REM ---- Parse target ----
set "TARGET=all"
if "%~1"=="" goto done_target
set "_arg=%~1"
if /i "%_arg%"=="sljit"  ( set "TARGET=sljit"  & shift & goto done_target )
if /i "%_arg%"=="asmjit" ( set "TARGET=asmjit" & shift & goto done_target )
if /i "%_arg%"=="mir"    ( set "TARGET=mir"    & shift & goto done_target )
if /i "%_arg%"=="all"    ( set "TARGET=all"    & shift & goto done_target )
echo Usage: %0 [--pg-config PATH] [--pgdata DIR] [--restart] [sljit^|asmjit^|mir^|all]
exit /b 1

:done_target

echo.
echo === pg_jitter install (%TARGET%) ===
echo   Target: %PKGLIBDIR%

REM ---- Build backend list ----
if /i "%TARGET%"=="all" (
    set "BACKENDS=sljit asmjit mir"
) else (
    set "BACKENDS=%TARGET%"
)

REM ---- Find and copy DLLs ----
set "MISSING=0"
for %%b in (%BACKENDS%) do (
    set "FOUND="
    set "LIB=pg_jitter_%%b.dll"
    for %%d in ("%SCRIPT_DIR%\build\pg%PG_MAJOR%" "%SCRIPT_DIR%\build\%%b" "%SCRIPT_DIR%\build") do (
        if exist "%%~d\!LIB!" (
            if not defined FOUND set "FOUND=%%~d\!LIB!"
        )
    )
    if defined FOUND (
        copy /y "!FOUND!" "%PKGLIBDIR%\" >nul
        echo   !LIB! installed
    ) else (
        echo ERROR: !LIB! not found in build\ -- run build.cmd %%b first
        set "MISSING=1"
    )
)
if "%MISSING%"=="1" exit /b 1

REM ---- Install meta-provider if available ----
set "META_FOUND="
for %%d in ("%SCRIPT_DIR%\build\pg%PG_MAJOR%" "%SCRIPT_DIR%\build\meta" "%SCRIPT_DIR%\build") do (
    if exist "%%~d\pg_jitter.dll" (
        if not defined META_FOUND set "META_FOUND=%%~d\pg_jitter.dll"
    )
)
if defined META_FOUND (
    copy /y "%META_FOUND%" "%PKGLIBDIR%\" >nul
    echo   pg_jitter.dll installed (meta-provider)
)

REM ---- Restart PostgreSQL (only with --restart) ----
if "%DO_RESTART%"=="1" (
    echo.
    "%PGBIN%\pg_ctl" -D "%PG_DATA%" status >nul 2>&1
    if !errorlevel! equ 0 (
        "%PGBIN%\pg_ctl" -D "%PG_DATA%" restart -l "%TEMP%\pg_jitter.log"
    ) else (
        echo PostgreSQL is not running. Starting...
        "%PGBIN%\pg_ctl" -D "%PG_DATA%" start -l "%TEMP%\pg_jitter.log"
    )

    REM Show status
    set "PORT=5432"
    for /f "usebackq delims=" %%p in (`"%PGBIN%\psql" -d postgres -t -A -c "SHOW port;" 2^>nul`) do set "PORT=%%p"
    for /f "usebackq delims=" %%j in (`"%PGBIN%\psql" -p !PORT! -d postgres -t -A -c "SHOW jit_provider;" 2^>nul`) do set "PROVIDER=%%j"
    echo.
    echo Active jit_provider: !PROVIDER!
    echo.
    echo To switch: psql -p !PORT! -c "ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';"
)

echo.
echo === Done ===

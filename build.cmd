@echo off
setlocal enabledelayedexpansion
REM build.cmd -- Build pg_jitter backends (Windows / MSVC)
REM
REM Usage:
REM   build.cmd [options] [sljit^|asmjit^|mir^|meta^|all] [cmake args...]
REM
REM Options:
REM   --pg-config PATH   Path to pg_config.exe
REM   --generator NAME   CMake generator (default: Ninja if available, else NMake)
REM
REM Examples:
REM   build.cmd                                           Build all backends
REM   build.cmd sljit                                     Build sljit only
REM   build.cmd --pg-config C:\pg18\bin\pg_config all     Custom PG install
REM   build.cmd sljit -DPG_JITTER_USE_LLVM=ON            sljit with LLVM blobs
REM   build.cmd mir -DMIR_DIR=C:\mir                      Custom MIR path
REM
REM Prerequisites:
REM   - Run from a Visual Studio Developer Command Prompt (vcvarsall x64),
REM     or have cl.exe, link.exe, and Windows SDK in PATH/INCLUDE/LIB.
REM   - CMake and Ninja (optional) on PATH.
REM   - pg_config on PATH or passed via --pg-config.
REM
REM pg_config resolution (first match wins):
REM   1. --pg-config argument
REM   2. PG_CONFIG environment variable
REM   3. pg_config from PATH
REM
REM Dependency paths (override with -D flags):
REM   -DSLJIT_DIR=...   Path to sljit source    (default: ..\sljit)
REM   -DASMJIT_DIR=...  Path to asmjit source   (default: ..\asmjit)
REM   -DMIR_DIR=...     Path to MIR source       (default: ..\mir-patched)

set "SCRIPT_DIR=%~dp0"
REM Remove trailing backslash
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---- Parse --pg-config / --generator if present ----
set "PG_CONFIG_ARG="
set "GENERATOR="

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
if /i "%~1"=="--generator" (
    if "%~2"=="" (
        echo ERROR: --generator requires a name argument
        exit /b 1
    )
    set "GENERATOR=%~2"
    shift
    shift
    goto parse_args
)
goto done_parse

:done_parse

REM ---- Resolve pg_config to absolute path ----
if defined PG_CONFIG_ARG (
    set "PG_CONFIG=%PG_CONFIG_ARG%"
) else if not defined PG_CONFIG (
    where pg_config >nul 2>&1
    if !errorlevel! equ 0 (
        for /f "delims=" %%p in ('where pg_config') do set "PG_CONFIG=%%p"
    ) else (
        echo ERROR: pg_config not found.
        echo   Use: build.cmd --pg-config C:\path\to\pg_config.exe
        echo    or: set PG_CONFIG=C:\path\to\pg_config.exe
        exit /b 1
    )
)

REM Verify pg_config works
"%PG_CONFIG%" --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: pg_config failed: %PG_CONFIG%
    exit /b 1
)

REM ---- Verify MSVC environment ----
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not found. Run from a Visual Studio Developer Command Prompt.
    echo   e.g.: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    exit /b 1
)

REM ---- Resolve CMake generator ----
if not defined GENERATOR (
    where ninja >nul 2>&1
    if !errorlevel! equ 0 (
        set "GENERATOR=Ninja"
    ) else (
        set "GENERATOR=NMake Makefiles"
    )
)

REM ---- Parse target (first remaining arg if it matches a backend name) ----
set "TARGET=all"
set "CMAKE_EXTRA_ARGS="

if "%~1"=="" goto done_target
set "_arg=%~1"
if /i "%_arg%"=="sljit"  ( set "TARGET=sljit"  & shift & goto collect_extra )
if /i "%_arg%"=="asmjit" ( set "TARGET=asmjit" & shift & goto collect_extra )
if /i "%_arg%"=="mir"    ( set "TARGET=mir"    & shift & goto collect_extra )
if /i "%_arg%"=="meta"   ( set "TARGET=meta"   & shift & goto collect_extra )
if /i "%_arg%"=="all"    ( set "TARGET=all"    & shift & goto collect_extra )

:collect_extra
:extra_loop
if "%~1"=="" goto done_target
set "_token=%~1"
REM cmd.exe treats '=' as an argument delimiter, splitting -DVAR=VALUE into
REM two tokens (-DVAR and VALUE).  Detect -D tokens and rejoin with the
REM following token so cmake receives the flag correctly.
REM NOTE: shift inside a parenthesized block does NOT update %1-%9 until the
REM block exits, so we must use goto to break out before reading the next token.
if "!_token:~0,2!"=="-D" goto :extra_rejoin_d
set "CMAKE_EXTRA_ARGS=!CMAKE_EXTRA_ARGS! !_token!"
shift
goto extra_loop

:extra_rejoin_d
shift
set "CMAKE_EXTRA_ARGS=!CMAKE_EXTRA_ARGS! !_token!=%~1"
shift
goto extra_loop

:done_target

REM ---- Map target to backend list ----
set "BACKENDS_ARG="
if /i "%TARGET%"=="sljit"  set "BACKENDS_ARG=-DPG_JITTER_BACKENDS=sljit"
if /i "%TARGET%"=="asmjit" set "BACKENDS_ARG=-DPG_JITTER_BACKENDS=asmjit"
if /i "%TARGET%"=="mir"    set "BACKENDS_ARG=-DPG_JITTER_BACKENDS=mir"
if /i "%TARGET%"=="meta"   set "BACKENDS_ARG=-DPG_JITTER_BACKENDS="

REM ---- Determine PG version for per-version build directory ----
for /f "tokens=2 delims= " %%v in ('"%PG_CONFIG%" --version') do set "PG_FULL_VER=%%v"
for /f "tokens=1 delims=." %%m in ("%PG_FULL_VER%") do set "PG_MAJOR=%%m"

set "BUILD_DIR=%SCRIPT_DIR%\build\pg%PG_MAJOR%"

echo.
echo === Building pg_jitter (%TARGET%) for PostgreSQL %PG_MAJOR% ===
echo     Generator: %GENERATOR%

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -B "%BUILD_DIR%" -S "%SCRIPT_DIR%" -G "%GENERATOR%" ^
    -DPG_CONFIG="%PG_CONFIG%" ^
    %BACKENDS_ARG% ^
    %CMAKE_EXTRA_ARGS%

if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --parallel

if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo === Done ===
echo Output: %BUILD_DIR%
dir /b "%BUILD_DIR%\*.dll" 2>nul

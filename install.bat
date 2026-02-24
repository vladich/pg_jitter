@echo off
REM install.bat — Install pg_jitter backends (Windows)
REM
REM Usage:
REM   install.bat [sljit|asmjit|mir|all]
REM
REM Examples:
REM   install.bat              Install all 3
REM   install.bat sljit        Install sljit only
REM   set PG_CONFIG=C:\pg18\bin\pg_config.exe & install.bat

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if not defined PG_CONFIG set "PG_CONFIG=pg_config"

REM Get PG pkglibdir
for /f "usebackq delims=" %%i in (`"%PG_CONFIG%" --pkglibdir`) do set "PKGLIBDIR=%%i"
if not defined PKGLIBDIR (
    echo ERROR: Could not determine pkglibdir from %PG_CONFIG%
    exit /b 1
)

REM Parse target
set "TARGET=all"
if not "%~1"=="" (
    if /i "%~1"=="sljit"  set "TARGET=sljit"
    if /i "%~1"=="asmjit" set "TARGET=asmjit"
    if /i "%~1"=="mir"    set "TARGET=mir"
    if /i "%~1"=="all"    set "TARGET=all"
)

echo === pg_jitter install (%TARGET%) ===
echo   Target: %PKGLIBDIR%

REM Build list of backends
if /i "%TARGET%"=="all" (
    set "BACKENDS=sljit asmjit mir"
) else (
    set "BACKENDS=%TARGET%"
)

REM Check and copy DLLs
set "MISSING=0"
for %%b in (%BACKENDS%) do (
    set "DLL=%SCRIPT_DIR%\build\%%b\Release\pg_jitter_%%b.dll"
    if not exist "!DLL!" (
        echo ERROR: !DLL! not found — run build.bat %%b first
        set "MISSING=1"
    )
)
if "%MISSING%"=="1" exit /b 1

echo.
for %%b in (%BACKENDS%) do (
    copy /y "%SCRIPT_DIR%\build\%%b\Release\pg_jitter_%%b.dll" "%PKGLIBDIR%\" >nul
    echo   pg_jitter_%%b.dll installed
)

echo.
echo === Install complete ===
echo.
echo Restart PostgreSQL to load the new provider.
echo To switch: psql -c "ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';"

exit /b 0

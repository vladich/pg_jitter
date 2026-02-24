@echo off
REM build.bat — Build pg_jitter backends (Windows)
REM
REM Usage:
REM   build.bat [sljit|asmjit|mir|all] [cmake args...]
REM
REM Examples:
REM   build.bat                                              Build all 3
REM   build.bat sljit                                        Build sljit only
REM   build.bat sljit -DPG_JITTER_USE_LLVM=ON                sljit with LLVM blobs
REM   build.bat mir -DMIR_DIR=C:\src\mir                     Custom MIR path
REM   set PG_CONFIG=C:\pg18\bin\pg_config.exe & build.bat    Custom PG install
REM
REM Dependency paths (override with -D flags):
REM   -DPG_CONFIG=...   Path to pg_config
REM   -DSLJIT_DIR=...   Path to sljit source
REM   -DASMJIT_DIR=...  Path to asmjit source
REM   -DMIR_DIR=...     Path to MIR source

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

if not defined PG_CONFIG set "PG_CONFIG=pg_config"

REM Parse target
set "TARGET=all"
set "EXTRA_ARGS="
if "%~1"=="" goto :do_build
if /i "%~1"=="sljit"  (set "TARGET=sljit"  & shift & goto :collect_args)
if /i "%~1"=="asmjit" (set "TARGET=asmjit" & shift & goto :collect_args)
if /i "%~1"=="mir"    (set "TARGET=mir"    & shift & goto :collect_args)
if /i "%~1"=="all"    (set "TARGET=all"    & shift & goto :collect_args)
goto :collect_args

:collect_args
if "%~1"=="" goto :do_build
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto :collect_args

:do_build
if /i "%TARGET%"=="all" (
    call :build_one sljit %EXTRA_ARGS%
    if errorlevel 1 exit /b 1
    call :build_one asmjit %EXTRA_ARGS%
    if errorlevel 1 exit /b 1
    call :build_one mir %EXTRA_ARGS%
    if errorlevel 1 exit /b 1
) else (
    call :build_one %TARGET% %EXTRA_ARGS%
    if errorlevel 1 exit /b 1
)

echo.
echo === Done ===
exit /b 0

:build_one
set "NAME=%~1"
shift
set "BUILD_DIR=%SCRIPT_DIR%\build\%NAME%"
set "CMAKE_ARGS="
:collect_build_args
if "%~1"=="" goto :run_cmake
set "CMAKE_ARGS=%CMAKE_ARGS% %1"
shift
goto :collect_build_args

:run_cmake
echo.
echo === Building %NAME% ===
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

cmake "%SCRIPT_DIR%\cmake" ^
    -DPG_CONFIG="%PG_CONFIG%" ^
    -DBACKEND=%NAME% ^
    %CMAKE_ARGS%
if errorlevel 1 exit /b 1

cmake --build . --target pg_jitter_%NAME% --config Release -j
if errorlevel 1 exit /b 1

echo   Built: %BUILD_DIR%\Release\pg_jitter_%NAME%.dll
exit /b 0

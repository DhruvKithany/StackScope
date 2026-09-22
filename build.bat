@echo off
REM Build script for C0 Virtual Machine on Windows (MinGW/GCC)

if "%1"=="clean" goto clean
if "%1"=="test" goto test

echo [*] Compiling C0 Virtual Machine...
gcc -std=c11 -Wall -Wextra -Iinclude -O2 -o c0vm.exe src/c0vm_main.c src/bc0_reader.c src/c0vm.c src/c0_native.c
if %ERRORLEVEL% NEQ 0 (
    echo [-] Build failed!
    exit /b %ERRORLEVEL%
)
echo [+] Successfully built c0vm.exe
goto end

:test
call :build
echo [*] Running full test suite...
python run_all_tests.py
goto end

:clean
if exist c0vm.exe del c0vm.exe
if exist *.o del *.o
if exist tests\*.out del tests\*.out
if exist tests\c0\*.bc0 del tests\c0\*.bc0
if exist examples\*.bc0 del examples\*.bc0
echo [*] Clean complete.
goto end

:end

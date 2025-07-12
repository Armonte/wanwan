@echo off
echo Building FM2K GekkoNet Test Launcher...

REM Set up compiler and flags
set COMPILER=g++
set CFLAGS=-std=c++17 -O2 -Wall
set INCLUDES=-I. -I./FM2KHook/src
set LIBS=-lSDL3 -lSDL3main

REM Build the test launcher
%COMPILER% %CFLAGS% %INCLUDES% test_networking.cpp FM2K_GameInstance.cpp -o test_networking.exe %LIBS%

if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    echo.
    echo Usage examples:
    echo   test_networking.exe 0 7000 127.0.0.1:7001
    echo   test_networking.exe 1 7001 127.0.0.1:7000
    echo.
    echo Make sure to:
    echo   1. Update the game executable path in test_networking.cpp
    echo   2. Build the DLL hook separately
    echo   3. Allow the application through Windows Firewall
) else (
    echo Build failed!
    echo.
    echo Make sure you have:
    echo   - SDL3 development libraries installed
    echo   - A C++17 compatible compiler
    echo   - The FM2K_GameInstance.h header file
)

pause
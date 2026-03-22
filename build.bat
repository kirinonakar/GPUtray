@echo off
setlocal enabledelayedexpansion

:: Get the directory of this script
set "ROOT_DIR=%~dp0"
cd /d "%ROOT_DIR%"

:: Check if CMake is installed
where cmake >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake is not found in your PATH. Please install it to build.
    pause
    exit /b 1
)

:: Handle clean argument
if "%1"=="clean" (
    echo Cleaning build directory...
    if exist "build" rd /s /q "build"
)

:: Create build directory
if not exist "build" mkdir "build"

:: Enter build directory
cd build

:: Configure and Build
echo Configuring CMake...
cmake ..
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo Building Project (Release)...
cmake --build . --config Release
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo ===================================================
echo  Build Successful!
echo  Executable: build\Release\GpuTray.exe
echo ===================================================
echo.

pause

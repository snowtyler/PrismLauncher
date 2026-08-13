@echo off
setlocal enabledelayedexpansion

:: Default build configuration
set "BUILD_CONFIG=Debug"
if not "%~1"=="" (
    set "BUILD_CONFIG=%~1"
)

echo ===================================================
echo Prism Launcher Build ^& Installer Generator
echo Config: %BUILD_CONFIG%
echo ===================================================

:: Detect Visual Studio
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do (
    set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
    echo [ERROR] Visual Studio could not be located.
    exit /b 1
)

echo [INFO] Found Visual Studio at: %VS_PATH%

:: Set up MSVC environment for x64
set "VS_DEV_CMD=%VS_PATH%\Common7\Tools\VsDevCmd.bat"
if not exist "%VS_DEV_CMD%" (
    echo [ERROR] VsDevCmd.bat not found at %VS_DEV_CMD%
    exit /b 1
)

echo [INFO] Initializing Developer Command Prompt for x64...
call "%VS_DEV_CMD%" -arch=x64 -host_arch=x64

:: Add CMake and Ninja from VS to PATH in case they are not added
set "CMAKE_DIR=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA_DIR=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%CMAKE_DIR%;%NINJA_DIR%;C:\Program Files (x86)\NSIS;C:\Program Files\NSIS;%PATH%"

:: Reconfigure project with Updater enabled and Qt6 prefix path
echo [INFO] Configuring CMake project...
cmake --preset windows_msvc -DLauncher_BUILD_ARTIFACT="PrismLauncher" -DCMAKE_PREFIX_PATH="C:/Users/Tyler/6.10.3/msvc2022_64"

if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

:: Compile the project
echo [INFO] Compiling project (%BUILD_CONFIG%)...
cmake --build --preset windows_msvc --config %BUILD_CONFIG%

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed.
    exit /b %ERRORLEVEL%
)

:: Install binaries locally
echo [INFO] Installing built binaries to staging directory...
cmake --install build --config %BUILD_CONFIG%

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Installation to staging directory failed.
    exit /b %ERRORLEVEL%
)

:: Setup NSIS plugins (NScurl is required by win_install.nsi)
echo [INFO] Downloading and preparing NSIS plugins...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\download_plugins.ps1"

:: Detect makensis
set "MAKENSIS_EXE=makensis.exe"
echo [INFO] Using NSIS compiler at: %MAKENSIS_EXE%

:: Compile installer
echo [INFO] Building NSIS installer...
cd install
"%MAKENSIS_EXE%" -NOCD "..\build\program_info\win_install.nsi"
set "NSIS_ERROR=%ERRORLEVEL%"
cd ..

if %NSIS_ERROR% neq 0 (
    echo [ERROR] NSIS installer compilation failed with error code %NSIS_ERROR%.
    exit /b %NSIS_ERROR%
)

echo [SUCCESS] Build and packaging complete!
echo [SUCCESS] Installer file created at: PrismSync-Setup-12.0.0.exe

@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo   DEVICE (Prism Launcher Fork) - Build Utility
echo ===================================================
echo.

:: Locate vswhere.exe
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
)

if not exist "!VSWHERE!" (
    echo [ERROR] vswhere.exe not found. Visual Studio installation could not be detected automatically.
    echo Please make sure Visual Studio is installed.
    pause
    exit /b 1
)

:: Find the latest VS installation path
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -property installationPath`) do (
    set "VS_PATH=%%i"
)

if "!VS_PATH!"=="" (
    echo [ERROR] No Visual Studio installations found.
    pause
    exit /b 1
)

set "DEV_CMD=!VS_PATH!\Common7\Tools\VsDevCmd.bat"
if not exist "!DEV_CMD!" (
    echo [ERROR] Developer Command Prompt script not found at:
    echo "!DEV_CMD!"
    pause
    exit /b 1
)

echo [INFO] Found Visual Studio at: !VS_PATH!
echo [INFO] Initializing Developer Environment (x64)...
call "!DEV_CMD!" -arch=x64

:: Locate CMake
where cmake.exe >nul 2>nul
if %errorlevel% neq 0 (
    set "CMAKE_EXE=!VS_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else (
    set "CMAKE_EXE=cmake.exe"
)

if not exist "!CMAKE_EXE!" (
    echo [WARNING] cmake.exe not in PATH, checking VS built-in CMake...
    set "CMAKE_EXE=!VS_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not exist "!CMAKE_EXE!" (
    echo [ERROR] CMake could not be found. Please ensure CMake is installed.
    pause
    exit /b 1
)

echo [INFO] Found CMake at: "!CMAKE_EXE!"
echo [INFO] Starting build for Release configuration...
echo.

"!CMAKE_EXE!" --build build --config Release --parallel 16

if %errorlevel% equ 0 (
    echo.
    echo ===================================================
    echo   BUILD SUCCESSFUL!
    echo ===================================================
    echo.
    
    :: Ensure portable mode is enabled
    echo. > build\Release\portable.txt
    echo [INFO] Created portable.txt in build\Release
    
    :: Copy Java checker and launcher libraries
    if exist "build\jars" (
        echo [INFO] Copying Java libraries to build\Release\jars...
        xcopy /E /I /Y "build\jars" "build\Release\jars"
    ) else (
        echo [WARNING] build\jars directory not found. Java compatibility check might fail.
    )
    
    :: Locate windeployqt.exe
    set "WINDEPLOYQT_EXE=C:\Qt\6.11.1\msvc2022_64\bin\windeployqt.exe"
    if not exist "!WINDEPLOYQT_EXE!" (
        echo [INFO] Searching for windeployqt.exe in C:\Qt...
        for /r "C:\Qt" %%f in (windeployqt.exe) do (
            if exist "%%f" set "WINDEPLOYQT_EXE=%%f"
        )
    )
    
    if exist "!WINDEPLOYQT_EXE!" (
        echo [INFO] Found windeployqt at: "!WINDEPLOYQT_EXE!"
        echo [INFO] Deploying Qt6 DLLs and dependencies...
        "!WINDEPLOYQT_EXE!" build\Release\device.exe
        echo.
        echo ===================================================
        echo   DEPLOYMENT COMPLETE!
        echo   Executable is ready at: build\Release\device.exe
        echo ===================================================
    ) else (
        echo [WARNING] windeployqt.exe not found in C:\Qt.
        echo Please ensure Qt is installed and DLLs are deployed.
    )
) else (
    echo.
    echo ===================================================
    echo   BUILD FAILED! Please review compilation errors above.
    echo ===================================================
)

pause

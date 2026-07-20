@echo off
echo Configuring Visual Studio Developer Environment...
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
) else (
    echo Warning: Could not locate vcvarsall.bat automatically.
)

echo Configuring PATH with Visual Studio CMake and NSIS...
set "PATH=%PATH%;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\NSIS;C:\Program Files\NSIS"

echo Re-configuring CMake with explicit binary name...
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLauncher_APP_BINARY_NAME=prismsync

echo Building Prism Sync launcher...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo Build failed!
    exit /b %errorlevel%
)

echo Installing built binaries to staging directory...
cmake --install build --config Release
if %errorlevel% neq 0 (
    echo Installation failed!
    exit /b %errorlevel%
)

echo Packaging installer using NSIS...
if not exist install mkdir install
cd install
makensis -NOCD "..\build\program_info\win_install.nsi"
set "NSIS_ERROR=%errorlevel%"
cd ..

if %NSIS_ERROR% neq 0 (
    echo Packaging failed!
    exit /b %NSIS_ERROR%
)

echo Done! Setup file generated.

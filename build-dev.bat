@echo off
setlocal

rem Always run from the repository root, even when invoked from elsewhere.
pushd "%~dp0" || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe was not found. Install Visual Studio or Visual Studio Build Tools.
    popd
    exit /b 1
)

set "VS_INSTALLATION="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_INSTALLATION=%%i"
)

if not defined VS_INSTALLATION (
    echo ERROR: Visual Studio Build Tools with the C++ workload was not found.
    popd
    exit /b 1
)

call "%VS_INSTALLATION%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo ERROR: Failed to initialize the Visual Studio developer environment.
    popd
    exit /b 1
)

cmake --preset=dev
if errorlevel 1 (
    popd
    exit /b 1
)

cmake --build --preset=dev
set "BUILD_RESULT=%ERRORLEVEL%"

popd
exit /b %BUILD_RESULT%

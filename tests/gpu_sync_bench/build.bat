@echo off
rem Builds the GPU sync benchmark into build\gpu-sync-bench\Release.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake -S "%~dp0." -B "%~dp0..\..\build\gpu-sync-bench" -A x64
if errorlevel 1 exit /b 1
cmake --build "%~dp0..\..\build\gpu-sync-bench" --config Release
exit /b %errorlevel%

:no_vs
echo Visual Studio with the C++ toolset was not found.
echo Open a Developer Command Prompt and run:
echo   cmake -S "%~dp0." -B "%~dp0..\..\build\gpu-sync-bench" -A x64
echo   cmake --build "%~dp0..\..\build\gpu-sync-bench" --config Release
exit /b 1

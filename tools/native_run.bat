@echo off
setlocal EnableDelayedExpansion
rem Builds dantes_inferno_native (SDK Vulkan backend in-process) and launches it
rem with --renderer=native (log: tools\native_build.log). Extra args are forwarded.
cd /d "%~dp0.."
set "LOG=%CD%\tools\native_build.log"
echo [native] start %DATE% %TIME% > "%LOG%"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq tokens=*" %%i in (`vswhere -latest -products * -property installationPath`) do set "VSPATH=%%i"
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "VSCMAKE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=C:\Program Files\LLVM\bin;!VSCMAKE!\CMake\bin;!VSCMAKE!\Ninja;%PATH%"
echo Building dantes_inferno_native ^(log: tools\native_build.log^) ...
cmake --build out\build\win-amd64-release --target dantes_inferno_native >> "%LOG%" 2>&1
if errorlevel 1 goto :fail
echo [native] BUILD OK >> "%LOG%"
echo Launching game ...
start "" /D "%CD%" "%CD%\out\build\win-amd64-release\dantes_inferno_native.exe" --game_data_root=game --renderer=native %*
echo [native] launched %TIME% >> "%LOG%"
exit /b 0

:fail
echo [native] BUILD FAILED >> "%LOG%"
echo BUILD FAILED - see tools\native_build.log
exit /b 1

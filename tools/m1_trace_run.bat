@echo off
setlocal EnableDelayedExpansion
rem Builds dantes_inferno + the GPU plugin and launches the game (log: tools\m1_build.log).
cd /d "%~dp0.."
set "LOG=%CD%\tools\m1_build.log"
echo [m1] start %DATE% %TIME% > "%LOG%"
rem "(x86)" breaks inside for /f backquotes, so vswhere goes on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq tokens=*" %%i in (`vswhere -latest -products * -property installationPath`) do set "VSPATH=%%i"
echo [m1] VS at !VSPATH! >> "%LOG%"
rem Not into the log: vcvars' vctip.exe child would keep it locked.
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "VSCMAKE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=C:\Program Files\LLVM\bin;!VSCMAKE!\CMake\bin;!VSCMAKE!\Ninja;%PATH%"
echo Building dantes_inferno ^(log: tools\m1_build.log^) ...
cmake --build out\build\win-amd64-release --target dantes_inferno rexgpu-xenos >> "%LOG%" 2>&1
if errorlevel 1 goto :fail
rem The exe loads the plugin from its own folder.
set "PLUGIN_DST=out\build\win-amd64-release\rexgpu-xenos.dll"
if not exist "%PLUGIN_DST%.bak" copy /y "%PLUGIN_DST%" "%PLUGIN_DST%.bak" >> "%LOG%"
copy /y "thirdparty\rexglue-sdk\out\win-amd64\rexgpu-xenos.dll" "%PLUGIN_DST%" >> "%LOG%"
if errorlevel 1 goto :fail
echo [m1] BUILD OK >> "%LOG%"
echo Launching game ...
start "" /D "%CD%" "%CD%\out\build\win-amd64-release\dantes_inferno.exe" --game_data_root=game --vfetch_oob_trace=false
echo [m1] launched %TIME% >> "%LOG%"
exit /b 0

:fail
echo [m1] BUILD FAILED >> "%LOG%"
echo BUILD FAILED - see tools\m1_build.log
exit /b 1

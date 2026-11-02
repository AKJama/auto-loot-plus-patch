@echo off
rem Builds build\dinput8.dll from native\ with the compiler flags of BUILD.txt.
rem Dear ImGui and Vulkan-Headers are fetched into native\third_party at pinned commits (not in git).
rem Run from any prompt: it finds the MSVC x64 tools with vswhere when cl.exe is not on PATH.
setlocal
cd /d "%~dp0" || exit /b 1

where cl >nul 2>nul && goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_cl
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS goto :no_cl
call "%VCVARS%" >nul
where cl >nul 2>nul || goto :no_cl
:have_cl

call :fetch imgui https://github.com/ocornut/imgui.git v1.91.9b f5befd2d29e66809cd1110a152e375a7f1981f06 || exit /b 1
call :fetch vulkan https://github.com/KhronosGroup/Vulkan-Headers.git v1.4.309 952f776f6573aafbb62ea717d871cd1d6816c387 || exit /b 1

set "TP=%CD%\native\third_party"
if not exist build mkdir build
cd build || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /utf-8 /DNDEBUG /DIMGUI_IMPL_VULKAN_NO_PROTOTYPES /I"%TP%\imgui" /I"%TP%\vulkan\include" /LD ..\native\main.cpp ..\native\overlay.cpp "%TP%\imgui\imgui.cpp" "%TP%\imgui\imgui_draw.cpp" "%TP%\imgui\imgui_tables.cpp" "%TP%\imgui\imgui_widgets.cpp" "%TP%\imgui\backends\imgui_impl_vulkan.cpp" /link /DEF:..\native\exports.def /OUT:dinput8.dll kernel32.lib user32.lib || exit /b 1
echo Built build\dinput8.dll
exit /b 0

rem :fetch <folder> <url> <tag> <commit> - clone once, then always check the commit
:fetch
set "DIR=native\third_party\%~1"
if not exist "%DIR%\.git" git clone --quiet --depth 1 --branch %~3 %~2 "%DIR%" || exit /b 1
set "HEAD="
for /f %%h in ('git -C "%DIR%" rev-parse HEAD') do set "HEAD=%%h"
if /i not "%HEAD%"=="%~4" (
    echo %~1: commit %HEAD% is not the pinned %~4
    exit /b 1
)
exit /b 0

:no_cl
echo MSVC x64 tools not found: install Visual Studio Build Tools with "Desktop development with C++".
exit /b 1

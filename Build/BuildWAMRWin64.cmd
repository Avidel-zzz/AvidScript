@echo off
setlocal enabledelayedexpansion

set "PLUGIN_ROOT=%~dp0.."
for %%I in ("%PLUGIN_ROOT%") do set "PLUGIN_ROOT=%%~fI"

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "WAMR_ROOT=%PLUGIN_ROOT%\Source\ThirdParty\WAMR"
set "UPSTREAM_DIR=%WAMR_ROOT%\upstream"
set "BUILD_DIR=%WAMR_ROOT%\build\Win64\ReleaseNinja"
set "LIB_DIR=%WAMR_ROOT%\lib\Win64\Release"

if not exist "%VS_VCVARS%" (
  echo Missing Visual Studio vcvars64.bat: %VS_VCVARS%
  exit /b 1
)

if not exist "%UPSTREAM_DIR%\product-mini\platforms\windows\CMakeLists.txt" (
  echo Missing WAMR upstream Windows CMakeLists.txt under: %UPSTREAM_DIR%
  exit /b 1
)

call "%VS_VCVARS%"
if errorlevel 1 exit /b %ERRORLEVEL%

cmake -S "%UPSTREAM_DIR%\product-mini\platforms\windows" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 -DWAMR_BUILD_AOT=0 -DWAMR_BUILD_JIT=0 -DWAMR_BUILD_FAST_JIT=0 -DWAMR_BUILD_LIBC_BUILTIN=1 -DWAMR_BUILD_LIBC_WASI=0 -DWAMR_BUILD_MULTI_MODULE=0 -DWAMR_BUILD_SIMD=0 -DWAMR_BUILD_MINI_LOADER=0
if errorlevel 1 exit /b %ERRORLEVEL%

cmake --build "%BUILD_DIR%" --target vmlib
if errorlevel 1 exit /b %ERRORLEVEL%

if not exist "%LIB_DIR%" mkdir "%LIB_DIR%"

if exist "%BUILD_DIR%\iwasm.lib" copy /Y "%BUILD_DIR%\iwasm.lib" "%LIB_DIR%\iwasm.lib"
if exist "%BUILD_DIR%\libiwasm.lib" copy /Y "%BUILD_DIR%\libiwasm.lib" "%LIB_DIR%\libiwasm.lib"
if exist "%BUILD_DIR%\vmlib.lib" copy /Y "%BUILD_DIR%\vmlib.lib" "%LIB_DIR%\vmlib.lib"

if exist "%LIB_DIR%\iwasm.lib" exit /b 0
if exist "%LIB_DIR%\libiwasm.lib" exit /b 0
if exist "%LIB_DIR%\vmlib.lib" exit /b 0

echo WAMR static library was not found after build.
exit /b 1

@echo off
setlocal enabledelayedexpansion

set "PLUGIN_ROOT=%~dp0.."
for %%I in ("%PLUGIN_ROOT%") do set "PLUGIN_ROOT=%%~fI"

set "MAPPED_DRIVE=%AVIDSCRIPT_WAMR_BUILD_DRIVE%"
if not defined MAPPED_DRIVE set "MAPPED_DRIVE=R:"
if exist "%MAPPED_DRIVE%\nul" (
  echo WAMR build drive is already in use: %MAPPED_DRIVE%
  echo Set AVIDSCRIPT_WAMR_BUILD_DRIVE to an unused drive letter and retry.
  exit /b 1
)
subst %MAPPED_DRIVE% "%PLUGIN_ROOT%"
if errorlevel 1 exit /b %ERRORLEVEL%
set "MAPPING_ACTIVE=1"
set "RESULT=1"

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "WAMR_ROOT=%MAPPED_DRIVE%\Source\ThirdParty\WAMR"
set "UPSTREAM_DIR=%WAMR_ROOT%\upstream"
set "BUILD_DIR=%WAMR_ROOT%\build\Win64\ReleasePublic"
set "LIB_DIR=%WAMR_ROOT%\lib\Win64\Release"

if not exist "%VS_VCVARS%" (
  echo Missing Visual Studio vcvars64.bat: %VS_VCVARS%
  goto cleanup
)

if not exist "%UPSTREAM_DIR%\product-mini\platforms\windows\CMakeLists.txt" (
  echo Missing WAMR upstream Windows CMakeLists.txt under: %UPSTREAM_DIR%
  goto cleanup
)

call "%VS_VCVARS%"
if errorlevel 1 goto cleanup

cmake -S "%UPSTREAM_DIR%\product-mini\platforms\windows" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 -DWAMR_BUILD_AOT=0 -DWAMR_BUILD_JIT=0 -DWAMR_BUILD_FAST_JIT=0 -DWAMR_BUILD_LIBC_BUILTIN=1 -DWAMR_BUILD_LIBC_WASI=0 -DWAMR_BUILD_MULTI_MODULE=0 -DWAMR_BUILD_SIMD=0 -DWAMR_BUILD_MINI_LOADER=0
if errorlevel 1 goto cleanup

cmake --build "%BUILD_DIR%" --target vmlib
if errorlevel 1 goto cleanup

if not exist "%LIB_DIR%" mkdir "%LIB_DIR%"

if exist "%BUILD_DIR%\iwasm.lib" copy /Y "%BUILD_DIR%\iwasm.lib" "%LIB_DIR%\iwasm.lib"
if exist "%BUILD_DIR%\libiwasm.lib" copy /Y "%BUILD_DIR%\libiwasm.lib" "%LIB_DIR%\libiwasm.lib"
if exist "%BUILD_DIR%\vmlib.lib" copy /Y "%BUILD_DIR%\vmlib.lib" "%LIB_DIR%\vmlib.lib"

if exist "%LIB_DIR%\iwasm.lib" set "RESULT=0"
if exist "%LIB_DIR%\libiwasm.lib" set "RESULT=0"
if exist "%LIB_DIR%\vmlib.lib" set "RESULT=0"

if not "%RESULT%"=="0" echo WAMR static library was not found after build.

:cleanup
if defined MAPPING_ACTIVE subst %MAPPED_DRIVE% /d
exit /b %RESULT%

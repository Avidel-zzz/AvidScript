@echo off
setlocal enabledelayedexpansion
set "PATH=%SystemRoot%\System32;%PATH%"

set "PLUGIN_ROOT=%~dp0.."
for %%I in ("%PLUGIN_ROOT%") do set "PLUGIN_ROOT=%%~fI"

set "MAPPED_DRIVE=%AVIDSCRIPT_WAMR_BUILD_DRIVE%"
if not defined MAPPED_DRIVE set "MAPPED_DRIVE=R:"
if exist "%MAPPED_DRIVE%\nul" (
  echo WAMR build drive is already in use: %MAPPED_DRIVE%
  echo Set AVIDSCRIPT_WAMR_BUILD_DRIVE to an unused drive letter and retry.
  exit /b 1
)
"%SystemRoot%\System32\subst.exe" %MAPPED_DRIVE% "%PLUGIN_ROOT%"
if errorlevel 1 exit /b %ERRORLEVEL%
set "MAPPING_ACTIVE=1"
set "RESULT=1"

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "WAMR_ROOT=%MAPPED_DRIVE%\Source\ThirdParty\WAMR"
set "UPSTREAM_DIR=%WAMR_ROOT%\upstream"
set "BUILD_DIR=%WAMR_ROOT%\build\Win64\ReleasePublicSimdUrl"
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

cmake -S "%UPSTREAM_DIR%\product-mini\platforms\windows" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_INTERP=1 -DWAMR_BUILD_FAST_INTERP=1 -DWAMR_BUILD_AOT=0 -DWAMR_BUILD_JIT=0 -DWAMR_BUILD_FAST_JIT=0 -DWAMR_BUILD_LIBC_BUILTIN=1 -DWAMR_BUILD_LIBC_WASI=0 -DWAMR_BUILD_MULTI_MODULE=0 -DWAMR_BUILD_SIMD=1 -DWAMR_BUILD_LIB_SIMDE=1 -DWAMR_BUILD_MINI_LOADER=0 -DWAMR_BUILD_DUMP_CALL_STACK=1 -DWAMR_BUILD_WASM_C_API=0
if errorlevel 1 goto cleanup

cmake --build "%BUILD_DIR%" --target vmlib
if errorlevel 1 goto cleanup

if not exist "%LIB_DIR%" mkdir "%LIB_DIR%"

set "BUILT_LIB="
if exist "%BUILD_DIR%\iwasm.lib" set "BUILT_LIB=%BUILD_DIR%\iwasm.lib"
if exist "%BUILD_DIR%\libiwasm.lib" set "BUILT_LIB=%BUILD_DIR%\libiwasm.lib"
if exist "%BUILD_DIR%\vmlib.lib" set "BUILT_LIB=%BUILD_DIR%\vmlib.lib"
if not defined BUILT_LIB (
  echo WAMR static library was not found after build.
  goto cleanup
)

copy /Y "%BUILT_LIB%" "%LIB_DIR%\libiwasm.lib"
if errorlevel 1 goto cleanup

set "SYMBOLS_FILE=%BUILD_DIR%\libiwasm-linkermember.txt"
dumpbin /nologo /linkermember:1 "%LIB_DIR%\libiwasm.lib" > "%SYMBOLS_FILE%"
if errorlevel 1 goto cleanup

"%SystemRoot%\System32\findstr.exe" /r /c:" wasm_runtime_init$" "%SYMBOLS_FILE%" >nul
if errorlevel 1 (
  echo WAMR runtime symbol contract failed: wasm_runtime_init is missing.
  goto cleanup
)
"%SystemRoot%\System32\findstr.exe" /r /c:" wasm_runtime_load$" "%SYMBOLS_FILE%" >nul
if errorlevel 1 (
  echo WAMR runtime symbol contract failed: wasm_runtime_load is missing.
  goto cleanup
)

"%SystemRoot%\System32\findstr.exe" /c:" wasm_config_" /c:" wasm_engine_" /c:" wasm_functype_" /c:" wasm_trap_" "%SYMBOLS_FILE%" >nul
if not errorlevel 1 (
  echo WAMR symbol isolation failed: standard wasm-c-api symbols remain in libiwasm.lib.
  goto cleanup
)

set "RESULT=0"

:cleanup
if defined MAPPING_ACTIVE "%SystemRoot%\System32\subst.exe" %MAPPED_DRIVE% /d
exit /b %RESULT%

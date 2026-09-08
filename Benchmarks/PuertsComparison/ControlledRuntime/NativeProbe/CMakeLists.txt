cmake_minimum_required(VERSION 3.25)

project(AvidScriptWasmtimeNativeProfileProbe LANGUAGES C CXX)

if(NOT AVIDSCRIPT_ROOT)
    message(FATAL_ERROR "AVIDSCRIPT_ROOT must point to the AvidScript plugin root")
endif()

if(NOT AVIDSCRIPT_WASMTIME_INSTALL)
    message(FATAL_ERROR "AVIDSCRIPT_WASMTIME_INSTALL must point to the locked Wasmtime install")
endif()

add_executable(AvidScriptWasmtimeNativeProfileProbe
    NativeProfileProbe.cpp
    "${AVIDSCRIPT_ROOT}/Source/AvidScriptVM/Private/AvidScriptWasmtimeApi.c")

target_compile_features(AvidScriptWasmtimeNativeProfileProbe PRIVATE cxx_std_20)
target_include_directories(AvidScriptWasmtimeNativeProfileProbe PRIVATE
    "${AVIDSCRIPT_ROOT}/Source/AvidScriptVM/Private"
    "${AVIDSCRIPT_WASMTIME_INSTALL}/include")
target_link_libraries(AvidScriptWasmtimeNativeProfileProbe PRIVATE
    "${AVIDSCRIPT_WASMTIME_INSTALL}/lib/wasmtime.dll.lib")

if(MSVC)
    target_compile_options(AvidScriptWasmtimeNativeProfileProbe PRIVATE
        /O2 /EHsc /W3)
endif()

add_custom_command(TARGET AvidScriptWasmtimeNativeProfileProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AVIDSCRIPT_WASMTIME_INSTALL}/lib/wasmtime.dll"
        "$<TARGET_FILE_DIR:AvidScriptWasmtimeNativeProfileProbe>/wasmtime.dll")

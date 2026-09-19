cmake_minimum_required(VERSION 3.24)
project(AvidScriptManagedHeapTests LANGUAGES CXX)
add_executable(AvidScriptManagedHeapTests
    AvidScriptManagedHeapTests.cpp
    ../../Source/AvidScriptRuntime/Private/Memory/AvidScriptManagedHeap.cpp)
target_include_directories(AvidScriptManagedHeapTests PRIVATE ../../Source/AvidScriptRuntime/Private)
target_compile_features(AvidScriptManagedHeapTests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(AvidScriptManagedHeapTests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(AvidScriptManagedHeapTests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
enable_testing()
add_test(NAME ManagedHeap COMMAND AvidScriptManagedHeapTests)

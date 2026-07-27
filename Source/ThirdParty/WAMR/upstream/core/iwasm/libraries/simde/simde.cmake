# Copyright (C) 2024 Amazon Inc.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# simde is a header only library

# Yes. To solve the compatibility issue with CMAKE (>= 4.0), we need to update
# our `cmake_minimum_required()` to 3.5. However, there are CMakeLists.txt
# from 3rd parties that we should not alter. Therefore, in addition to
# changing the `cmake_minimum_required()`, we should also add a configuration
# here that is compatible with earlier versions.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 FORCE)

set (LIB_SIMDE_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions (-DWASM_ENABLE_SIMDE=1)

include_directories(${LIB_SIMDE_DIR} ${LIB_SIMDE_DIR}/simde)

include(FetchContent)

FetchContent_Declare(
    simde
    URL https://github.com/simd-everywhere/simde/archive/71fd833d9666141edcd1d3c109a80e228303d8d7.tar.gz
    URL_HASH SHA256=72b2c14a487560b7eb203795f2c2fead5c7499662e639944cca2a9bb19f09029
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

message("-- Fetching simde ..")
FetchContent_MakeAvailable(simde)
include_directories("${simde_SOURCE_DIR}")

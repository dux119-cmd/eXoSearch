cmake_minimum_required(VERSION 3.20)
project(EDS VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Fetch tinyxml2
include(FetchContent)
FetchContent_Declare(tinyxml2
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 9.0.0
)
set(tinyxml2_BUILD_TESTING OFF)
FetchContent_MakeAvailable(tinyxml2)

# Source files
set(SOURCES
    src/application.cpp
    src/display_manager.cpp
    src/input_handler.cpp
    src/safe_queue.cpp
    src/search_engine.cpp
    src/utilities.cpp
    src/xml_parser.cpp
    src/main.cpp
)

# Create executable
add_executable(eds ${SOURCES})
target_link_libraries(eds PRIVATE tinyxml2)

# Compiler warnings
target_compile_options(eds PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wformat=2>
)

# Size optimization for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Link-Time Optimization (most effective for size reduction)
    set_property(TARGET eds PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    
    # Size-focused compilation flags
    target_compile_options(eds PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/O1>
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Os -ffunction-sections -fdata-sections>
    )
    
    # Size-focused linker flags
    target_link_options(eds PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/LTCG /OPT:REF /OPT:ICF>
        $<$<CXX_COMPILER_ID:GNU>:-Wl,--gc-sections -Wl,--strip-all>
        $<$<CXX_COMPILER_ID:Clang>:-Wl,--gc-sections -Wl,--strip-all>
    )
    
    # Hide symbols by default (reduces export table)
    set_property(TARGET eds PROPERTY CXX_VISIBILITY_PRESET hidden)
    set_property(TARGET eds PROPERTY VISIBILITY_INLINES_HIDDEN ON)
endif()

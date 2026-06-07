cmake_minimum_required(VERSION 3.25)

project(
    forge
    VERSION 0.3.0
    DESCRIPTION "A project workflow system for C++"
    LANGUAGES CXX
)

option(FORGE_BUILD_TESTS "Build Forge tests" ON)

add_library(forge_core STATIC
    src/box.cpp
    src/bump.cpp
    src/build.cpp
    src/clean.cpp
    src/cli.cpp
    src/github.cpp
    src/init.cpp
    src/new.cpp
    src/process.cpp
    src/recipe.cpp
    src/release.cpp
    src/run.cpp
    src/runtime_assets.cpp
    src/sha256.cpp
    src/test.cpp
    src/zip.cpp
)
add_library(forge::core ALIAS forge_core)

target_compile_features(forge_core PUBLIC cxx_std_20)
target_include_directories(forge_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(MSVC)
    target_compile_options(forge_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(forge_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(forge
    src/main.cpp
)
target_link_libraries(forge PRIVATE forge::core)

install(TARGETS forge RUNTIME DESTINATION bin)

if(FORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

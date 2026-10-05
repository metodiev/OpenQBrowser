cmake_minimum_required(VERSION 3.21)

project(OpenQBrowser
    VERSION 0.1.0
    DESCRIPTION "An open-source web browser built from scratch"
    HOMEPAGE_URL "https://github.com/metodiev/OpenQBrowser"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(OPENQBROWSER_BUILD_TESTS "Build the OpenQBrowser test suites" ON)
option(OPENQBROWSER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Svg)
find_package(ZLIB REQUIRED)

qt_standard_project_setup()

# Shared (non-Qt) dependencies and helper interface.
add_library(oqb_project_options INTERFACE)
target_include_directories(oqb_project_options INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oqb_project_options INTERFACE Qt6::Core)
target_compile_definitions(oqb_project_options INTERFACE
    OPENQBROWSER_VERSION="${PROJECT_VERSION}"
    OPENQBROWSER_APP_NAME="${PROJECT_NAME}")

if(MSVC)
    target_compile_options(oqb_project_options INTERFACE /W4)
else()
    target_compile_options(oqb_project_options INTERFACE -Wall -Wextra -Wpedantic)
    if(OPENQBROWSER_WARNINGS_AS_ERRORS)
        target_compile_options(oqb_project_options INTERFACE -Werror)
    endif()
endif()

add_subdirectory(src)

if(OPENQBROWSER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "OpenQBrowser ${PROJECT_VERSION}")
message(STATUS "  Build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt ............... ${Qt6_VERSION}")
message(STATUS "  Tests ............ ${OPENQBROWSER_BUILD_TESTS}")
message(STATUS "  Output ........... ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")

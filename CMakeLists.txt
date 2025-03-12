cmake_minimum_required (VERSION 3.13)
project (ayatana-indicator-a11y C CXX)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    SET (CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "..." FORCE)
endif (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)

set (PROJECT_VERSION "24.4.2")
set (PACKAGE ${CMAKE_PROJECT_NAME})

# Options

option (ENABLE_WERROR "Treat all build warnings as errors" OFF)

set(CMAKE_BUILD_TYPE "Release")

if (ENABLE_WERROR)
    add_definitions ("-Werror")
endif ()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    add_definitions ("-Weverything")
else ()
    add_definitions ("-Wall")
endif ()

# Prerequisites

include (GNUInstallDirs)
find_package (PkgConfig REQUIRED)
include (CheckIncludeFile)
include (FindPkgConfig)
pkg_check_modules (SERVICE_DEPS REQUIRED glib-2.0>=2.36 gio-2.0>=2.36 accountsservice libayatana-common x11 xrandr)
include_directories (SYSTEM ${SERVICE_DEPS_INCLUDE_DIRS})

# Translation

set (GETTEXT_PACKAGE "ayatana-indicator-a11y")
add_definitions (-DGETTEXT_PACKAGE="${GETTEXT_PACKAGE}" -DLOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}")

# Build

include_directories (${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories (${CMAKE_CURRENT_BINARY_DIR}/include)
add_subdirectory (src)
add_subdirectory (data)
add_subdirectory (po)

# Info

message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Build with -Werror: ${ENABLE_WERROR}")

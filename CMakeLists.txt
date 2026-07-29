cmake_minimum_required(VERSION 3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

# ============================================================================
#   Configuration of DMOD
# ============================================================================
set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmftp VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "FTP for DMOD: the dmftp protocol library and the ftpd service"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)

dmod_setup_external_module()

# Author of every module in this repository
set(DMOD_AUTHOR_NAME "Mikolaj Filar")

enable_testing()

# ============================================================================
#   Subdirectories
#
#   This repo builds two modules (see README.md): the `dmftp` Library, which
#   holds the protocol itself, and the `ftpd` Application, a thin service
#   front-end that configures and runs a server from a unit file. Only a
#   Library module can be enabled as a dependency of another module, which is
#   why the protocol does not live inside `ftpd`.
# ============================================================================
add_subdirectory(app)

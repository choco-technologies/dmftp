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

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmftp VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmftp DMOD library module"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmftp)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Mikolaj Filar")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmftp.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # src/dmftp.c: the transport-agnostic RFC 959 control-line engine
    # (dmftp_create/_destroy/_recv/_reply - see include/dmftp.h).
    # src/dmftp_server.c: dmtcp wiring (control-connection listener,
    # connection table, PASV port bookkeeping) and config loading -
    # dmod_init()/dmod_deinit() live here, since dmftp is a Library-type
    # module started as a dmsystem "type=library" unit (see
    # configs/ftpd.ini and docs/service.md), the same shape dmicmp uses.
    # src/dmftp_commands.c: RFC 959 command handling, virtual-root path
    # resolution, and PASV data-connection transfer (LIST/NLST/RETR/STOR).
    src/dmftp.c
    src/dmftp_server.c
    src/dmftp_commands.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmtcp/dmip/dmroute/dmnetif: the control/data TCP connections themselves
#     (dmtcp_listen()/_listen_any()/_send()/... - dmip/dmroute/dmnetif are
#     needed transitively for dmip_addr_t/dmnetif_iface_t, the same reason
#     telnetd links all four for its own dmtcp usage - see
#     https://github.com/choco-technologies/dmtelnet's tools/telnetd).
# dmini: parses this server's own settings (port/root/user/pass) out of
#     the same unit file dmsystem starts it from - see
#     src/dmftp_server.c's load_config().
# dmosi: the mutex guarding the connection table and PASV port bookkeeping.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmtcp
    dmip
    dmroute
    dmnetif
    dmini
    dmosi
)

enable_testing()
add_subdirectory(tests)

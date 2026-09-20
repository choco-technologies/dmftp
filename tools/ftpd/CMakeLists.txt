# =====================================================================
#               ftpd - FTP server (Application)
# =====================================================================
# Name of the module
set(DMOD_MODULE_NAME        ftpd)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Mikolaj Filar")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         4096)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/ftpd.dmr)

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/ftpd.c
    src/ftpd_server.c
    src/ftpd_commands.c
)

# libftp is this same repository's own top-level module, already built as
# part of this CMake configure by the time tools/ is added (see the root
# CMakeLists.txt) - link its libftp_if target directly, exactly like
# tests/CMakeLists.txt does, instead of fetching libftp's own published
# headers back via dmod_link_modules() as if it were an external module.
target_link_libraries(${DMOD_MODULE_NAME} libftp_if)

# dmtcp/dmip/dmroute/dmnetif: the control/data TCP connections themselves
#     (dmtcp_listen()/_listen_any()/_connect()/_send()/... - dmip/dmroute/
#     dmnetif are needed transitively for dmip_addr_t/dmnetif_iface_t, the
#     same reason telnetd links all four for its own dmtcp usage - see
#     https://github.com/choco-technologies/dmtelnet's tools/telnetd).
# dmosi: the mutex guarding the connection table/PASV bookkeeping, and
#     dmosi_thread_sleep() for main()'s idle loop.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmtcp
    dmip
    dmroute
    dmnetif
    dmosi
)

# No tests/ here - ftpd is dmtcp/filesystem-backed integration glue,
# exercised on real hardware instead, exactly like telnetd.c (its closest
# analog in this ecosystem) has no test suite of its own either. libftp's
# actual protocol logic is what tests/ (at the repo root) covers.

# Name of the module
set(DMOD_MODULE_NAME        dmftp)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Mikolaj Filar")

# Stack size for the module (should be integer)
#
# dmftp runs entirely on its callers' stacks - dmtcp's rx thread and its
# on_writable callback - and never recurses; the transfer engine stages
# through heap buffers rather than stack ones. 2 KB covers the deepest path
# (a control command resolving a path and opening a file).
set(DMOD_STACK_SIZE         2048)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmftp.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#
# A Library, not an Application: `ftpd` (and anyone else embedding an FTP
# server or client) enables this as a required module, and only a Library
# module can be enabled as a dependency of another module.
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/dmftp.c
    src/dmftp_registrations.c
    src/dmftp_wire.c
    src/dmftp_path.c
    src/dmftp_net.c
    src/dmftp_xfer.c
    src/dmftp_server.c
    src/dmftp_server_cmd.c
    src/dmftp_server_xfer.c
    src/dmftp_client.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmtcp:   the whole transport - listen/connect/send plus the on_writable
#          callback the transfer pump is built on.
# dmip:    dmip_addr_t, which the public API uses for every address.
# dmroute: header-only - dmip_addr_t's real definition (dmroute_addr_t).
# dmnetif: header-only - dmnetif_iface_t, threaded through the accept path.
# dmlist:  the listener registry and each server's session list.
# dmosi:   the one recursive mutex this module serializes itself with.
#
# Files come from the dmod SAL instead, so dmvfs is deliberately absent -
# see include/dmftp.h.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmtcp
    dmip
    dmroute
    dmnetif
    dmlist
    dmosi
)

add_subdirectory(tests)

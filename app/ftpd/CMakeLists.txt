# Name of the module
set(DMOD_MODULE_NAME        ftpd)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Mikolaj Filar")

# Stack size for the module (should be integer)
#
# main() parses a configuration file and starts a server, then returns; the
# protocol work afterwards runs on dmtcp's threads with dmftp's own stack
# budget, not this one.
set(DMOD_STACK_SIZE         2048)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/ftpd.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

#
#   dmod_add_executable - create an executable module
#
# An Application: a unit file runs it (`exec=ftpd`), it configures and
# starts a dmftp server, and returns. Everything protocol-shaped lives in
# the dmftp Library next door.
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/ftpd.c
)

# dmini:  the INI configuration file.
# dmlist: the account table.
# dmip/dmroute/dmnetif: dmftp.h's own transitive types (dmip_addr_t and
#         friends) have to resolve for this translation unit too.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmini
    dmlist
    dmip
    dmroute
    dmnetif
)

# dmftp is a local sibling module built in this same repository, so its
# headers come straight from its own `_if` interface target rather than a
# dmod_link_modules() fetch (which is for genuinely external, separately
# published modules).
target_link_libraries(${DMOD_MODULE_NAME}
    dmftp_if
)

add_subdirectory(tests)

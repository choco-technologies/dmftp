set(DMOD_MODULE_NAME        test_dmftp)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Mikolaj Filar")
# The end-to-end steps run a whole dmnetbridge -> dmip -> dmtcp -> dmftp
# command dispatch on this module's stack, several frames deeper than the
# codec steps need, and each hop stages a packet in a local buffer.
set(DMOD_STACK_SIZE         16384)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmftp_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(${DMOD_MODULE_NAME} dmftp_if)

# dmip/dmroute/dmnetif: dmftp.h's public API is expressed in dmip_addr_t, so
#   these have to resolve for the test translation unit too.
# dmtcp/dmnetbridge/dmarp: the end-to-end steps build real IPv4/TCP frames and
#   push them into dmnetbridge's packet_received DIF, so a command travels the
#   same dmnetbridge -> dmip -> dmtcp -> dmftp path a real client's would (the
#   fixture shape dmtcp's own tests use, see tests/dmftp_test.c).
# dmosi:  dmosi_get_tick_count(), to predict dmtcp's tick-derived ISS.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmtcp
    dmnetbridge
    dmarp
    dmip
    dmroute
    dmnetif
    dmosi
)

# Register the test module with ctest - this replays the same two steps CI
# runs: install the test module's dependencies with dmf-get, then execute it
# through dmod_loader. The test's exit code equals its number of failed steps
# (see dmod_add_test).
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()

# #############################################################################
#
# 	The dmftp library module - the FTP protocol itself (server and client).
# 	The Make-based equivalent of this directory's CMakeLists.txt.
#
# #############################################################################
DMOD_DIR=@DMOD_DIR@

# -----------------------------------------------------------------------------
#  Paths initialization
# -----------------------------------------------------------------------------
include $(DMOD_DIR)/paths.mk

# -----------------------------------------------------------------------------
#   Module configuration
# -----------------------------------------------------------------------------

# The name of the module
DMOD_MODULE_NAME=dmftp

# The version of the module
DMOD_MODULE_VERSION=0.1

# The name of the author
DMOD_AUTHOR_NAME=Mikolaj Filar

# The list of C sources
#
# dmftp_registrations.c must stay a translation unit of its own - see the
# comment at the top of that file.
DMOD_CSOURCES=src/dmftp.c \
              src/dmftp_registrations.c \
              src/dmftp_wire.c \
              src/dmftp_path.c \
              src/dmftp_net.c \
              src/dmftp_xfer.c \
              src/dmftp_server.c \
              src/dmftp_server_cmd.c \
              src/dmftp_server_xfer.c \
              src/dmftp_client.c

# The list of C++ sources
DMOD_CXXSOURCES=

# The list of include directories
DMOD_INC_DIRS=include

# The list of libraries to link
DMOD_LIBS=

# The list of definitions
DMOD_DEFINITIONS=

# -----------------------------------------------------------------------------
#   List of MAL interfaces implemented by the module
# -----------------------------------------------------------------------------
DMOD_MAL_IMPLS=

# -----------------------------------------------------------------------------
#   List of DIF interfaces implemented by the module
# -----------------------------------------------------------------------------
DMOD_DIF_IMPLS=

# -----------------------------------------------------------------------------
#   Include the dmod app makefile
# -----------------------------------------------------------------------------
include $(DMOD_DMF_LIB_FILE_PATH)

# #############################################################################
#
# 	The ftpd application module - the FTP service front-end over dmftp.
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
DMOD_MODULE_NAME=ftpd

# The version of the module
DMOD_MODULE_VERSION=0.1

# The name of the author
DMOD_AUTHOR_NAME=Mikolaj Filar

# The list of C sources
DMOD_CSOURCES=src/ftpd.c

# The list of C++ sources
DMOD_CXXSOURCES=

# The list of include directories
#
# ../dmftp/include: dmftp is a sibling module in this same repository, so its
# public header comes straight from the source tree rather than from a fetch.
DMOD_INC_DIRS=../dmftp/include

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
include $(DMOD_DMF_APP_FILE_PATH)

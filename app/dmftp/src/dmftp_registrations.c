/*
 * Registers dmftp's Built-in API in the .dmod.inputs section - the same
 * shape dmtcp_registrations.c/dmdhcp_registrations.c use for their own
 * multi-file modules.
 *
 * This must live in its own translation unit, separate from every other
 * dmftp_*.c file: the registration struct array dmftp_defs.h generates when
 * DMOD_ENABLE_REGISTRATION is set covers every function declared in
 * dmftp.h, not just the ones defined in whichever file set the macro - so
 * defining it in more than one translation unit produces one duplicate
 * "multiple definition" linker error per public function. Only dmftp.h is
 * included here (not the full dmod.h) so this cannot accidentally
 * re-register dmod's own kernel Built-in APIs too.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmftp.h"

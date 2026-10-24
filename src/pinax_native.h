// pinax_native.h - what the C that `pnx export-c` writes needs (RFC-0028): the instruction functions
// of the interpreter (pinax_ops.h, static inline) and the few functions of the library it calls
// (pnx_step, pnx_exec_end, pnx_exec_suspend). ./nob dist writes it as one file beside pinax.h and
// pinax.c; inside the repository it is this file. It belongs to the build that wrote it: code
// exported by another build carries another hash and is not attached.
#ifndef PINAX_NATIVE_H
#define PINAX_NATIVE_H
#include "pinax_ops.h"
#endif

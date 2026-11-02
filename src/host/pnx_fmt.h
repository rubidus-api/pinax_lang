// pnx_fmt.h - the formatter of the command pnx.
#ifndef PNX_FMT_H
#define PNX_FMT_H

#include "proven/allocator.h"

#include <stddef.h>
#include <stdio.h>

// Writes `text` to `out` with its lines laid out the one way. `name` is the file's name, for
// the message when the text is not Pinax. The tokens are kept in memory from `alloc`. Returns
// 0, 1 when the text does not lex, or 2 when there is no memory for its tokens.
int pnx_format(proven_allocator_t alloc, const char *text, size_t length, FILE *out, const char *name);

#endif // PNX_FMT_H

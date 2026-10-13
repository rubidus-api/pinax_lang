// pnx_fmt.h - the formatter of the command pnx.
#ifndef PNX_FMT_H
#define PNX_FMT_H

#include <stddef.h>
#include <stdio.h>

// Writes `text` to `out` with its lines laid out the one way. `name` is the file's name, for
// the message when the text is not Pinax. Returns 0, or 1 when the text does not lex.
int pnx_format(const char *text, size_t length, FILE *out, const char *name);

#endif // PNX_FMT_H

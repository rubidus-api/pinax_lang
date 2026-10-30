// pnx_streams.h - pnx's own standard streams in the host modules (D0068). Not part of
// pinax_host.h: a host that embeds the library keeps io writing and reading without a buffer.
#ifndef PNX_STREAMS_H
#define PNX_STREAMS_H
#include <stdbool.h>

// From now on io.print and io.println keep what they write in a buffer, written when it is full,
// before io.eprint and clock.sleep, before io.read_line asks the system for more of standard
// input, and at pnx_streams_flush; with read_ahead,
// io.read_line reads standard input ahead (for pnx when it does not read standard input itself).
void pnx_streams_own(bool read_ahead);
// Writes what standard output holds back; pnx calls it every time a run returns to it.
void pnx_streams_flush(void);

#endif

/*
 * pinax_host.h - the standard host modules of Pinax (RFC-0009 section 5, RFC-0015).
 *
 * Ordinary host words written against pinax.h alone. A host links this file's library only
 * when it wants them, registers the modules it chooses, and may replace any of them with its
 * own. Unlike the core they use the operating system.
 *
 *   io      print println eprint (s str)          read_line (gives line str or fail)
 *   file    open_read open_write (path str gives owned file or fail)
 *           write (s str, f file or fail)          read_line (f file gives line str or fail)
 *           read_all (path str gives body str or fail)        close (f owned file)
 *           read (f file, buf buffer gives n int or fail): appends to the buffer; 0 at the end
 *   The words that make a str in the region have buffer forms that append to a buffer instead:
 *   io.read_line_into (buf buffer or fail), file.read_line_into (f file, buf buffer or fail),
 *   file.read_all_into (path str, buf buffer or fail). At the end of the input they fail with
 *   not_found, and with full when the buffer has no room for the line or the file.
 *   clock   now_ms monotonic_ns (gives int)        sleep (ms int)
 *   random  random (gives int)   random_below (n int gives int)   seed (n int)
 *   env     arg_count (gives int)   arg (i int gives s str or fail)   getenv (name str gives s str or fail)
 */
#ifndef PINAX_HOST_H
#define PINAX_HOST_H

#include "pinax.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers one module by name ("io", "file", "clock", "random", "env"), before code is
 * loaded. PINAX_ERR_NOT_FOUND for another name. */
pinax_err pinax_host_register(pinax_state *state, const char *module);
/* Registers all five. */
pinax_err pinax_host_register_all(pinax_state *state);
/* What env.arg_count and env.arg report. The strings must outlive the states that read them. */
void pinax_host_set_args(int count, const char *const *args);

/* A file mapped read-only and shared with every process that maps it, for pinax_share_file().
 * The bytes start at a page. Unmap it after the states that read it are closed. An empty file is
 * PINAX_ERR_FORMAT; a file that cannot be opened is PINAX_ERR_NOT_FOUND or PINAX_ERR_IO. */
typedef struct pinax_mapping pinax_mapping;
pinax_err pinax_map_file(const char *path, pinax_str *bytes, pinax_mapping **out);
void      pinax_unmap_file(pinax_mapping *mapping);

#ifdef __cplusplus
}
#endif

#endif /* PINAX_HOST_H */

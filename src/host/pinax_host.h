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
 *   sqlite  (RFC-0043) see pinax_host_register_sqlite below
 *   env     arg_count (gives int)   arg (i int gives s str or fail)   getenv (name str gives s str or fail)
 */
#ifndef PINAX_HOST_H
#define PINAX_HOST_H

#include "pinax.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers one module by name ("io", "file", "clock", "random", "env", "sh"), before code is
 * loaded. PINAX_ERR_NOT_FOUND for another name. "sh" runs commands through the system shell
 * (RFC-0040 part B): register it only for scripts that may; its pipes are files of "file". */
pinax_err pinax_host_register(pinax_state *state, const char *module);
/* Registers the first five (not "sh"), and pinax_host_barrier as the state's barrier. */
pinax_err pinax_host_register_all(pinax_state *state);
/* The module "sqlite" (RFC-0043): SQLite as words, in a part of this library of its own. It is
 * not registered by pinax_host_register_all, and there is no name for it in pinax_host_register:
 * a host that never calls this function does not link SQLite. A script that may use it may read and
 * write any database file the process can open (also by ATTACH): register it as you would "file".
 * (pnx built with PNX_SQLITE=0 has no sqlite: ./nob reads that variable.) */
pinax_err pinax_host_register_sqlite(pinax_state *state);
/* Process-wide memory limits of SQLite in bytes: a soft one (caches shrink) and a hard one (an
 * allocation past it fails as err.full). 0 is no limit; a negative value leaves it as it is.
 * Defaults 64 MiB and 256 MiB. May be called before or after registering. */
void pinax_host_sqlite_limits(int64_t soft_bytes, int64_t hard_bytes);
/* What a dot that ends a statement finishes for these modules (pinax_on_barrier): the output
 * they hold back is written. `user` is not used. */
void pinax_host_barrier(void *user);
/* After a run or a load that sh.exit stopped (it answers PINAX_ERR_INTERRUPTED): true, and the
 * status asked for (0 to 255). It tells it once, and of the calling thread: asked again, or after
 * an interruption of another cause, it is false. */
bool pinax_host_exit_asked(int *status);
/* What env.arg_count and env.arg report. The strings must outlive the states that read them. */
void pinax_host_set_args(int count, const char *const *args);

/* A file mapped read-only and shared with every process that maps it, for pinax_share_file().
 * The bytes start at a page. Unmap it after the states that read it are closed. An empty file is
 * PINAX_ERR_FORMAT; a file that cannot be opened is PINAX_ERR_NOT_FOUND, or PINAX_ERR_FAILED for
 * another reason. */
typedef struct pinax_mapping pinax_mapping;
pinax_err pinax_map_file(const char *path, pinax_str *bytes, pinax_mapping **out);
void      pinax_unmap_file(pinax_mapping *mapping);

#ifdef __cplusplus
}
#endif

#endif /* PINAX_HOST_H */

# Vendored SQLite

A copied snapshot of the SQLite amalgamation. Do not edit files here; to update, fetch a newer
amalgamation, check its SHA3-256 against sqlite.org/download.html, copy `sqlite3.c` and `sqlite3.h`,
change this file, and run the tests.

- Version: 3.54.1, `sqlite-amalgamation-3540100.zip` (2,990,966 bytes), retrieved 2026-11-13 from
  https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256 of the zip: `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` (equal to
  the value listed on https://www.sqlite.org/download.html)
- Copied: `sqlite3.c`, `sqlite3.h`. Not copied: `shell.c`, `sqlite3ext.h` (no extensions are loaded).
- License: public domain (see `LICENSE-NOTE.md`).
- Use: the host module `sqlite` (`src/host/pinax_sqlite.c`, RFC-0043), in `pnx` and in
  `libpinax_host.a` only; never in the core, the one-file `pinax.c` or the web page.
- Build: `nob.c` compiles `sqlite3.c` apart from Pinax's flags, with `-DSQLITE_THREADSAFE=2
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_DEFAULT_MEMSTATUS=1 -DSQLITE_USE_ALLOCA -DSQLITE_DEFAULT_FOREIGN_KEYS=1
  -DSQLITE_ENABLE_MATH_FUNCTIONS`, and keeps the object (it is large).

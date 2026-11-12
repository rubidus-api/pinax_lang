#import "/tools/common.typ": *
// shape: host words follow the rules of clauses 9 to 12; their refusals are shown there (12-owned-*)

= Standard host modules <host-modules>

#para("1")[The modules of this clause are host words that come with the processor. They are not part of the language:
they exist only when a host registers them (13.3). `pnx` registers all seven. A host that embeds the library picks them
one by one with `pinax_host_register(state, "io")` and the like (library `libpinax_host`). Using a module that was not
registered is refused (13.1).]

#para("2")[Host words keep the same rules as other words: their signatures are checked at load, words with `or fail`
shall be guarded, and `owned` values are handed over exactly once.]

#para("3")[What the operating system refuses is a failure (`or fail`). A missing file and the end of an input are
`err.not_found`, bytes of the wrong form `err.format`, no room `err.full`, and any other refusal (permission, a device
gone) `err.failed`. A word without `or fail` (`print`, `close`) does not report the operating system's refusal.]

== `io` --- standard input and output <host-io>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`print` `println`], [`(s str)`], [writes to standard output; `println` adds a line break (LF)],
  [`eprint`], [`(s str)`], [writes to standard error],
  [`read_line`], [`(gives line str or fail)`], [one line of standard input, without its line break; `err.not_found` at the end of input],
  [`read_line_into`], [`(buf buffer or fail)`], [appends one line to a buffer],
)
Bytes are written unchanged; the same bytes come out on every operating system.]

#ex("spec", "17-io.pnx")

== `file` --- files <host-file>

#para("1")[The handle type `file` is an open file. The opening words give an `owned file` and `close` takes it (12.4).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`open_read` `open_write`], [`(path str gives owned file or fail)`], [opens for reading; for writing (made new or emptied)],
  [`write`], [`(s str, f file or fail)`], [writes bytes],
  [`read_line`], [`(f file gives line str or fail)`], [one line; `err.not_found` at the end],
  [`read`], [`(f file, buf buffer gives n int or fail)`], [reads into a buffer and gives the number of bytes read],
  [`read_line_into`], [`(f file, buf buffer or fail)`], [appends one line to a buffer],
  [`read_all`], [`(path str gives body str or fail)`], [reads a whole file into the region],
  [`read_all_into`], [`(path str, buf buffer or fail)`], [appends a whole file to a buffer],
  [`close`], [`(f owned file)`], [closes it],
)
A missing file is `err.not_found`.]

#ex("spec", "17-file.pnx")

== `clock` `random` `env` <host-misc>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`clock.now_ms`], [`(gives int)`], [the time now, in milliseconds since 1970-01-01 UTC],
  [`clock.monotonic_ns`], [`(gives int)`], [nanoseconds of a clock that never goes back],
  [`clock.sleep`], [`(ms int)`], [waits; the state does nothing else meanwhile],
  [`random.seed`], [`(n int)`], [sets the sequence; the same seed gives the same sequence],
  [`random.random`], [`(gives int)`], [a 64-bit random number (not for cryptography)],
  [`random.random_below`], [`(n int gives int)`], [from 0 to `n`−1],
  [`env.arg_count`], [`(gives int)`], [the number of arguments; 0 is the program file],
  [`env.arg`], [`(i int gives s str or fail)`], [argument `i`],
  [`env.getenv`], [`(name str gives s str or fail)`], [an environment variable; `err.not_found` when unset],
)]

#ex("spec", "17-env.pnx")
#ex("spec", "17-random-clock.pnx")

== Channels and the host loop <host-channel>

#para("1")[A #term("channel") is a line of byte messages, up to a set number of them. The host names a channel and
registers it in a state as a module of that name (`pinax_channel_register`, library `libpinax_host`). The states that
registered the same channel pass messages to each other. They share no memory: a message is copied in and copied out.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`send`], [`(msg str or fail)`], [puts a message in; waits while the channel is full],
  [`receive`], [`(gives msg str or fail)`], [takes the message that came first; waits while it is empty],
  [`close`], [`()`], [no more messages go in],
)]

#para("2")[`send` fails with `err.full` when the message is longer than the channel allows, and with `err.cancelled`
once the channel is closed. `receive` fails with `err.not_found` on a channel that is closed and empty. Messages left in
a closed channel are still taken.]

#para("3")[Only the task that called waits (14.2); the other tasks of the state go on. Only runs the host loop runs
(`pinax_loop_start`) wait. In any other run, a `send` or `receive` that would have to wait ends the run with the fault
`PINAX_ERR_STATE`.]

#para("4")[The #term("host loop") runs states on a set number of threads. `pinax_loop_open` takes a configuration:
the number of threads, how many runs it holds at once, how much a host operation's results may take, and one block of
memory, as a state takes one: the loop's objects are fixed slots in it (`pinax_loop_size` bytes), and each channel
carves its slots from the same block when it is opened (`pinax_channel_size` bytes). Nothing is allocated while runs
go on; with no block the loop takes one piece of that size from proven's heap. A state runs on one thread at a time. A run
whose budget (19.5) is used up goes to the back of the line, and a run that waits gives its thread up. While the host
waits in `pinax_loop_wait`, when every run left waits for a channel and no host operation is outstanding, the loop ends
all of them with `PINAX_ERR_DEADLOCK`.]

#ex("spec", "17-channel.c")

== `sh` --- commands <host-sh>

#para("1")[`sh` runs commands through the system shell (`/bin/sh -c`; `cmd.exe /c` on Windows). `pnx` registers it; a
host that embeds the library registers it only for scripts that may run commands (`pinax_host_register(state,
"sh")`; `pinax_host_register_all` does not), and the WebAssembly host has no `sh`.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`sh.run`], [`(cmd str gives status int or fail)`], [runs `cmd`; its output goes where the program's goes, after what the program wrote. The status is the command's: a status other than 0 is not a failure. Fails only when no shell can be started],
  [`sh.output`], [`(cmd str gives out str or fail)`], [runs `cmd` and gives what it wrote on standard output, as written (with its line breaks), in the region],
  [`sh.output_into`], [`(cmd str, buf buffer gives status int or fail)`], [the same, appended to a buffer; `err.full` when it does not fit (the command is read to its end)],
  [`sh.open_read`], [`(cmd str gives owned file or fail)`], [a pipe from the command's standard output, as a `file` (17.2): `file.read_line`, `file.read`, `file.close`],
  [`sh.open_write`], [`(cmd str gives owned file or fail)`], [a pipe to the command's standard input: `file.write`, `file.close`],
  [`sh.status`], [`(gives int)`], [the status of the last command that ended: by `run`, `output`, `output_into`, or a pipe's `file.close`, which waits for its command],
  [`sh.quote`], [`(s str gives str)`], [`s` as one word for the system shell, to build a command from data: in single quotes for `/bin/sh`; in double quotes for `cmd.exe`, where `%` and `!` keep their meaning],
  [`sh.cd`], [`(path str or fail)`], [changes the working directory of the process; `err.not_found` when it cannot],
  [`sh.cwd`], [`(gives path str or fail)`], [the working directory],
  [`sh.setenv`], [`(name str, value str or fail)`], [sets an environment variable for the commands that follow (and `env.getenv`)],
  [`sh.exit`], [`(status int)`], [ends the program with that status (0 to 255; others are clamped), after what is held back is written. The run stops there; in `pnx repl` the session ends],
)]

#ex("spec", "17-sh.pnx")

#para("2")[A status is the command's exit code; a command ended by a signal has 128 plus the signal's number. A
command that cannot be found is a status (127 for `/bin/sh`), not a failure. A command with a zero byte in it is
`err.argument`. A write to a pipe whose command has stopped reading fails (`err.failed`). `sh.status` and `sh.exit`
are of the thread that runs the state.]

== `sqlite` --- databases <host-sqlite>

#para("1")[`sqlite` opens SQLite databases (files, or `":memory:"`) and runs SQL. `pnx` registers it; a host that embeds
the library registers it with `pinax_host_register_sqlite(state)`, a function of its own: `pinax_host_register_all` does
not, `pinax_host_register` has no name for it, and a host that never calls it does not link SQLite. The WebAssembly host
has no `sqlite`, and a `pnx` built without it (`PNX_SQLITE=0`) refuses `use sqlite`. A script that may use it may read and
write any database file the process can open (also by `ATTACH`): register it as `file` is registered. A connection
is the handle `sqlite` and a statement the handle `stmt`; both are `owned` when made and closed once (`sqlite.close`,
`sqlite.finalize`).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`sqlite.open`], [`(path str gives owned sqlite or fail)`], [opens the database, creating it when it is not there; `":memory:"` is a database in memory and `""` a private temporary one. A path is a path: no `file:` names],
  [`sqlite.open_read`], [`(path str gives owned sqlite or fail)`], [opens it read only; `err.not_found` when it cannot be opened],
  [`sqlite.close`], [`(db owned sqlite)`], [closes the connection. A transaction left open is rolled back. Statements made on it keep working until they are finalized; the database closes with the last one],
  [`sqlite.exec`], [`(sql str, db sqlite or fail)`], [runs the statements of `sql`, one after the other, and drops any rows. For text the program wrote; values go through `bind_*`],
  [`sqlite.scalar_int`], [`(sql str, db sqlite gives n int or fail)`], [the first column of the first row, which must be an integer (`err.format`); `err.not_found` when there is no row],
  [`sqlite.scalar_str`], [`(sql str, db sqlite gives s str or fail)`], [the same as a text (an integer or a real is given in its text form; a null or a blob is `err.format`)],
  [`sqlite.changes`], [`(db sqlite gives n int)`], [the rows the last statement that wrote changed],
  [`sqlite.last_id`], [`(db sqlite gives n int)`], [the row number the last insert made],
  [`sqlite.in_transaction`], [`(db sqlite gives bool)`], [whether a transaction is open],
  [`sqlite.busy_ms`], [`(ms int, db sqlite or fail)`], [how long a locked database is waited for; 0 (the start) does not wait],
  [`sqlite.prepare`], [`(sql str, db sqlite gives owned stmt or fail)`], [one statement. Text after it that is more than space and comments, and a text with no statement, is `err.format`],
  [`sqlite.finalize`], [`(s owned stmt)`], [],
  [`sqlite.reset`], [`(s stmt or fail)`], [runs the statement from the start again; the values bound stay],
  [`sqlite.clear_binds`], [`(s stmt)`], [binds null to every place],
  [`sqlite.bind_null`], [`(i int, s stmt or fail)`], [binds place `i`, counted from 0. A place that is not there is `err.range`; a statement that is running (not reset since its last row or its end) is `err.state`],
  [`sqlite.bind_int` `sqlite.bind_str` `sqlite.bind_blob`], [`(v int, i int, s stmt or fail)` and `(v str, i int, s stmt or fail)`], [bind an integer, a text or bytes (copied)],
  [`sqlite.bind_name`], [`(name str, s stmt gives i int or fail)`], [the place of `:name`, `@name` or `$name` (written with its mark); `err.not_found` when there is none],
  [`sqlite.step`], [`(s stmt gives more bool or fail)`], [takes one step: `true` when a row is there, `false` at the end],
  [`sqlite.col_count`], [`(s stmt gives n int)`], [the columns of the result],
  [`sqlite.col_name`], [`(i int, s stmt gives name str or fail)`], [the name of column `i` (from 0); `err.range` outside],
  [`sqlite.col_kind`], [`(i int, s stmt gives k int)`], [the kind of the value in column `i` of the current row: `sqlite.k_null`, `k_int`, `k_real`, `k_text` or `k_blob` (pure words). With no row it is `k_null`. A column that is not there is the fault `range`],
  [`sqlite.col_is_null`], [`(i int, s stmt gives bool)`], [whether it is null],
  [`sqlite.col_int`], [`(i int, s stmt gives n int or fail)`], [an integer column; any other kind is `err.format`, no row `err.not_found`],
  [`sqlite.col_str` `sqlite.col_blob`], [`(i int, s stmt gives v str or fail)`], [a text, or bytes, in the region. `col_str` also gives an integer or a real in its text form; `col_blob` takes only a blob],
  [`sqlite.col_str_into` `sqlite.col_blob_into`], [`(i int, s stmt, buf buffer or fail)`], [the same appended to a buffer; `err.full` when it does not fit],
  [`sqlite.last_code`], [`(gives n int)`], [the SQLite result code (extended, for example 2067 for a unique key) of the last refusal of the thread that runs the state],
  [`sqlite.last_error`], [`(gives msg str)`], [SQLite's text for it (at most 255 bytes)],
)]

#ex("spec", "17-sqlite.pnx")

#para("2")[A refusal of SQLite is a failure with one of these codes: `err.full` (out of memory, the disk full, a value too
large), `err.not_found` (a file that cannot be opened, no row), `err.range` (a place that is not there), `err.format` (a
file that is not a database, a statement that does not parse, a column of another kind), `err.state` (a statement used while
it runs), and `err.failed` for the rest (a busy or locked database, a constraint, a read only database, an input or output
error); `sqlite.last_code` tells which. This edition has no float type, so there are no words for a real: a real column
is read as a text (`sqlite.col_str`) and moved with `str.to_dec` (7.5), and money is bound as an integer in its smallest
unit.]

#para("3")[The memory SQLite uses is limited for the process: past a hard limit (256 MiB unless the host sets another)
an allocation fails as `err.full`. A connection is used by one thread at a time, as the state it belongs to is. A call
that waits for the disk holds the thread; no word is awaitable. A connection opens with defensive settings: no
extensions are loaded, a database cannot be damaged by `PRAGMA`, and a statement is at most 16 MiB of text.]

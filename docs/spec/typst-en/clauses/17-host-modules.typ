#import "/tools/common.typ": *
// shape: host words follow the rules of clauses 9 to 12; their refusals are shown there (12-owned-*)

= Standard host modules <host-modules>

#para("1")[The modules of this clause are host words that come with the processor. They are not part of the language:
they exist only when a host registers them (13.3). `pnx` registers all five. A host that embeds the library picks them
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

#para("4")[The #term("host loop") runs states on a set number of threads. A state runs on one thread at a time. A run
whose budget (19.5) is used up goes to the back of the line, and a run that waits gives its thread up. While the host
waits in `pinax_loop_wait`, when every run left waits for a channel and no host operation is outstanding, the loop ends
all of them with `PINAX_ERR_DEADLOCK`.]

#ex("spec", "17-channel.c")

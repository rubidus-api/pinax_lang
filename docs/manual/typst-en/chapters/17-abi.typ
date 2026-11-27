#import "/tools/common.typ": *
// shape: a reference chapter: the promise of the C interface between releases; its code is the link lines and checks

= ABI and compatibility <abi>

#opening[Chapter 14. A chapter to look things up in when you upgrade Pinax under a host.][
- I compiled my host against one release. Will it run with a later library?
- What do I link, and how do I check at start that the library is the right one?
- What may my callbacks call?
]

== Four numbers

#table(columns: (auto, 1fr, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Number*], [*What it says*], [*It changes when*]),
  [`PINAX_VERSION_MAJOR` `_MINOR` `_PATCH`], [the release], [every release],
  [`PINAX_FORMAT`], [sources, images and share files], [one of those formats changes in a way that breaks],
  [the container version], [what `pinax_load_bytecode` reads (internal)], [the container changes in a way that breaks],
  [`PINAX_ABI`], [the binary interface of `pinax.h`, `pinax_host.h` and `pinax_loop.h`], [a host compiled against the old header could misbehave with the new library],
)

A host compiled against release N runs, without being compiled again, with every later library of the same `PINAX_ABI`.
A release that raises `PINAX_ABI` says so in its notes under "Breaking", with what a host must change.

== The check at start

```c
if (!PINAX_ABI_OK()) {
    /* this library is not one this host was compiled for */
}
```

`PINAX_ABI_OK()` asks the library for its `pinax_abi()` and `pinax_version()`: the ABI must be the header's, and the
version at least the header's (a host that uses something new needs a library that has it).

== What does not break, and what does

#table(columns: (1fr, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Does not break*], [*Breaks (raises `PINAX_ABI`)*]),
  [a new function; a new flag bit; a new error code at the end of its range; a new field at the end of a struct that
   begins with `struct_size`; a call refused that the rules call a breach (the table below)],
  [a function removed or renamed; a parameter or result of another type; a field moved, removed or of another type; a
   struct without `struct_size` of another size; an enum value or a flag bit changed],
)

Every error code and flag has its number written in the header. A flag bit the library does not know is refused with
`PINAX_ERR_ARGUMENT`, so a host built against a later header learns it at once. A struct that begins with `struct_size`
is taken at the sizes its header lists, nothing in between; `pinax_stats_get` answers a size it does not know with
`struct_size` 0. `./nob abi` compares the header and the library with the last release's, on six targets, and names
the rule a change breaks.

== What to link

#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Form*], [*Link line*]),
  [the core, static], [`cc host.c build/native/release/libpinax.a -lm`],
  [with the host modules], [`... release/libpinax_host.a release/libpinax.a -lm -pthread`],
  [with `sqlite`], [`... release/libpinax_sqlite.a release/libpinax_host.a release/libpinax.a -lm -pthread`],
  [one file], [`cc -std=c23 -c pinax.c`, then link `pinax.o`],
  [shared (Linux)], [`./nob shared`: `cc host.c -Lbuild/native/shared -lpinax` (soname `libpinax.so.1`)],
  [shared (Windows)], [compile with `-DPINAX_SHARED`; link `libpinax.dll.a` or `pinax.def`; ship `pinax.dll`],
)

In what a host links only public names (`pinax_`) are global, and the core's own names are `pinax__`. proven_c_lib and
SQLite inside are private, so a host may link its own copies beside them. The library is built with GCC or Clang; a
host compiled with MSVC uses the DLL.

#mistake[Exported code linked against the shared library][
The C of `pnx export-c` calls the core's own functions (`pinax__...`), which change with every release. Link it with the
static archives or `pinax.c` of the release it was exported with, never with the shared library.
]

Images and exported code are for the same layout only: another build is refused (`PINAX_ERR_FORMAT`, or the table is
not attached).

== What a callback may call

#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Inside*], [*The state may be called with*]),
  [a host procedure], [`pinax_call_...`, `pinax_find` with `PINAX_FIND_NESTED`, `pinax_global_find`, `pinax_run` of a word so found, the handle functions, the functions that read],
  [the barrier, trace, task fault and `diag` functions], [the functions that read: `pinax_error_text`, `pinax_needed`, `pinax_fault_local`, `pinax_task_faults`, `pinax_eval_count`, `pinax_eval_value`, `pinax_word_at`, `pinax_image_size`, `pinax_share_size`, `pinax_stats_get`],
  [a handle type's close function], [`pinax_handle_get`, `pinax_handle_release`],
  [the `extern` resolver], [nothing],
)

Any other call is refused (`PINAX_ERR_STATE`, 0, `NULL`, or nothing done), and the error text names the callback.
`pinax_close` is never called from a callback.

#qa[How do I let a script I do not trust call a few C functions?][
Set a typed resolver (`pinax_set_typed_resolver`). It answers each function with its C layout, and a script that
declares another is refused at load. Give it only functions that are safe for any value of their arguments.
]

== Upgrading a host

+ Build the new release; run `./nob contract` with your compiler.
+ Read the release notes for "Breaking"; if `PINAX_ABI` changed, compile the host again and make the changes listed.
+ Keep `PINAX_ABI_OK()` at start.
+ Export C again (`pnx export-c`) for the new release; images of the old one are not opened.

#summary[
- `PINAX_ABI` names the binary interface; within one, an old host runs with a newer library.
- `PINAX_ABI_OK()` checks the library at start.
- Link `release/`: only `pinax_` names are global; proven and SQLite inside are private.
- Callbacks may call only what the table allows; the rest is refused.
- Exported code and images belong to one build.
]

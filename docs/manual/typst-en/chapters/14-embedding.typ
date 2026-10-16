#import "/tools/common.typ": *
// shape: C examples; the refusals and faults they meet are printed in their own output

= Embedding in a C program <embedding>

#opening[The language of chapters 1 to 13. Knowledge of C is assumed.][
- How does a C program open Pinax and call a script?
- Who decides what a script can do outside itself?
- How are scripts that are not trusted run?
]

== Open, register, load, call

A host has four jobs:
+ give a block of memory and open a state with `pinax_open`; everything of the state is inside that block;
+ register the #strong[host words] scripts may use with `pinax_register`, with signatures written as in Pinax;
+ load the script with `pinax_load`; when it is refused, the reason is in `pinax_error_text`;
+ find a word, with its signature, using `pinax_find`, and call it with `pinax_run`.

#ex("manual", "14-host.c")

Arguments and results pass in arrays by type: integers in `args->i[]` and `args->ri[]`, strings in `args->s[]` and
`args->rs[]`. When a host word returns something other than `PINAX_OK`, a word whose signature has `or fail` fails (the
script takes it with `or_else`), and a word without it becomes a fault.

#qa[Can a script delete files or reach the network?][Not unless the host registered words that do. The standard modules
do not reach outside; everything outside is a host word. Calling C functions with `extern` reaches only the functions
the host's resolver allows.]

== Scripts that are not trusted

`config.budget` is the number of instructions run at a time. When it is used up the run stops and returns
`PINAX_SUSPENDED`, and the host continues it with `pinax_resume` or gives up. While a run is stopped, the state takes no
other run. To give up, call `pinax_abort`: it ends the run as a fault would, closing the handles the run opened. What the
script stored in globals until then stays.

#ex("manual", "14-guard.c")

#mistake[A new `pinax_run` replaces a stopped run][It does not. A new run is refused with `PINAX_ERR_STATE`. Either
continue the stopped run to its end or give it up with `pinax_abort`. When even the globals must go back to how they
were before the run, save an image before running and open the state again from it (standard 19.9).]

== In a web page and in node

`./nob wasm` builds the core as the WebAssembly module `build/wasm/pinax.wasm` and puts the JavaScript module
`pinax.mjs` beside it. It needs clang and wasm-ld and nothing else; no C library is used. `createPinax(bytes, { write })`
of `pinax.mjs` opens the module, and `run(source, name)` of the object it gives loads the source into a fresh state and
runs the code outside definitions. It gives `{ ok: true }` on success and `{ ok: false, error }` otherwise.
`write(stream, text)` receives what `io.print` and `io.println` print on stream 1, and `io.eprint`, load errors and a
fault's line and locals on stream 2, in the same form as `pnx`.

In this edition the WebAssembly host registers one module, `io`; `io.read_line` always fails with `not_found`. There is
no `file`, `clock`, `random`, `env` or `extern`. The modules of the core (`str`, `json`, `csv`, `rex` and the rest) and
tasks run as everywhere.

#mistake[On WebAssembly, tasks run on several cores][They do not. Tasks run one after the other inside one state,
everywhere (chapter 13). The WebAssembly build uses the same core, and the same script prints the same thing.]

== More

Handles (giving a script a C object to hold), binding (host memory as globals), host words that finish later, images
and clones, and `extern` are in clauses 18 and 19 of the standard, with examples.

#summary[
- Give a block and open, register words, load, find and call (standard 19.2--19.5).
- A failing host word is a failure with `or fail`, a fault without (19.3).
- What a script can do outside itself is only what the host registered (13.3, 18).
- While a run stopped by the budget remains, new runs are refused; go back to an image (19.5, 19.9).
]

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

#qa[How big must the block be?][Opening takes about 150 KB with the defaults. Most of it is the main run's region (64 KB)
and the integer bank (32 KB), which `config.region_bytes` and `config.int_slots` can shrink. While a load runs, what is
left of the block is the compiler's working memory: about 20 KB for a small source, about 1 KB per word, and 64 KB more
when there is a constant pattern. When the load ends only the program stays and the working memory is free again. The
256 KB of the examples is plenty for a short script; for a long one, after `PINAX_ERR_NO_ROOM`, open again with at
least the size `pinax_needed` gives. That size is a lower bound, so it may fall short once more.]

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

This is what the processor guarantees. Whatever the source --- wrong, or written to do harm --- and whatever bytecode
passes the checks of `pinax_load_bytecode`, a script cannot read or write outside the state's block and the memory the
host gave it. With a budget other than 0, one run always comes back within a bounded number of instructions. A wrong
script is refused at load or stops with a fault; it does not bring the host down. The bytes of an image, a share file
and memory bound with `pinax_bind`, however, are data the host trusts; the processor checks only their shape.

#mistake[A new `pinax_run` replaces a stopped run][It does not. A new run is refused with `PINAX_ERR_STATE`. Either
continue the stopped run to its end or give it up with `pinax_abort`. When even the globals must go back to how they
were before the run, save an image before running and open the state again from it (standard 19.9).]

== On several threads

One state runs on one thread at a time. To use several cores, open several states. The host loop of the host library
(`pinax_loop.h`) runs states on a set number of threads, and #strong[channels] carry messages between them. The loop is
opened with a configuration, as a state is: the number of threads, how many states it holds at once, and a block of
memory that holds the loop and its channels; two size functions say how large. Without a block it takes one piece
of the heap of that size. The host hands a state and a word to the loop
with `pinax_loop_start` and hears that it ended through the function it gave
there. The host names a channel and registers it as a module; the script writes `use orders` and then `orders.send`,
`orders.receive` and `orders.close`. A task that puts into a full channel or takes from an empty one waits, and its
thread runs other states meanwhile.

#ex("manual", "14-threads.c")

A message is bytes. A row of records is packed with `bytes.pack` and the receiver unpacks it with `bytes.unpack`
(chapter 11), so both sides declare the same record. When a closed channel is empty, `receive` fails with `not_found`,
and the receiver reads that failure as the end.

#qa[Can two states use the same global?][Not one that changes. When several states change the same value, the race
cannot be caught at load. What changed is passed on in the messages of channels. When every run waits for a channel and
nothing is left to send, the loop ends those runs with `PINAX_ERR_DEADLOCK` (standard 17.4).]

Large data that is only read (a dictionary, a rate table, a word list and its index) is not loaded into every state;
one copy is read by all. One state loads the data, and the states that read it write `shared` in front of the same
declarations. When the host lends them the data state's storage with `pinax_share`, they read the same bytes without a
copy. Code that changes a shared global is refused at load, and the state that lent its storage neither runs nor loads
from then on. Since the bytes do not change, several threads read them together without locks.

#ex("manual", "14-shared.c")

An index is given after its table, since the index's slots point at the table's rows.

Programs that run as several processes share a file instead. The program that makes the data writes it with
`pinax_share_save`; each process maps it with `pinax_map_file` and gives its states the globals with
`pinax_share_file`. The operating system keeps one copy of the file's pages for all of them (standard 19.7).

#mistake[A shared list cannot be passed to a word that adds it up][It can, as long as the word only reads that
parameter. The processor sees whether a word changes a collection parameter, itself or through the words it passes it
to, and refuses only a shared collection given to such a word (standard 8.13).]

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
- For several cores, open several states and run them on the host loop; connect them with the messages of channels
  (17.4).
- Data that is only read is declared `shared` and read as one copy through `pinax_share` (8.13, 19.7).
- Several processes map one file written with `pinax_share_save` (`pinax_map_file`) and read it together (19.7).
]

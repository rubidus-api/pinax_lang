#import "/tools/common.typ": *
// shape: the C interface; its examples are programs that run

= The host interface <host>

#para("1")[The host interface is one C header, `pinax.h`. It uses only `<stddef.h>` and `<stdint.h>` and reads as C99,
C23 and C++. This clause defines what its functions do; the comments in the header give more detail for each.]

#para("2")[One state is used by one thread at a time; states share nothing. A `pinax_str` a function returns is valid
until the next call on that state unless stated otherwise.]

== Error codes <host-errors>

#para("1")[Functions return a `pinax_err`; `PINAX_OK` is 0. Three outcomes are not failures: `PINAX_SUSPENDED` (the
budget ran out, 19.5), `PINAX_WAITING` (every task waits for the host, 19.8) and `PINAX_PENDING` (a host word finishes
later). From `0x100` come the faults and failures of scripts (the codes of 11.1); from `0x200` the failures of the
interface itself: `ARGUMENT`, `NO_ROOM` (the block is too small), `SYNTAX`, `TYPE` (load refusals), `NOT_FOUND`,
`SIGNATURE`, `FORMAT`, `STATE` (a call not allowed now).]

#para("2")[`pinax_error_text` gives the last failure as text; for a fault, in the form `module:line: name`.]

#para("3")[A failure of the interface means the same in every function.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Code*], [*Meaning*],
  [`PINAX_ERR_ARGUMENT`], [an argument is wrong: a null pointer, a word number that is none, a size other than the declaration's],
  [`PINAX_ERR_NO_ROOM`], [the block has no room; `pinax_needed` says at least how big it had to be (19.3)],
  [`PINAX_ERR_SYNTAX` `PINAX_ERR_TYPE`], [the source was refused; the place and the reason are in `diag` and `pinax_error_text`],
  [`PINAX_ERR_NOT_FOUND`], [no word, global, module or host word of that name; also a host word not yet given again to a state opened from an image],
  [`PINAX_ERR_SIGNATURE`], [the signature differs: of the word looked for, of the host word registered, of the global shared],
  [`PINAX_ERR_FORMAT`], [a container, image or share file is not of this processor's form, or failed its checks],
  [`PINAX_ERR_STATE`], [a call not possible now: a run is suspended, a host word is running, no code is loaded, the state is frozen],
)]

== Opening and closing <host-open>

#para("1")[`pinax_open(block, config, &state)` opens a state inside `block`. Fields of `config` that are 0 take the
processor's defaults: the slots of the banks, the size of the main run's region, the number of handles alive at once, the
instruction budget of a run (0 for none), the function that receives load diagnostics. When the block is too small the
answer is `PINAX_ERR_NO_ROOM`, and `pinax_needed` tells at least the size that would have been enough: the size up to
what was being taken when the block ran out. A load may take more after that, so when the block opened again at that
size runs out too, read it again.]

#para("2")[`config.hash_key` is the 16-byte key of the hash indexes use (8.10). A host that indexes text from outside
gives a random key. All zeros means a fixed key.]

#para("3")[`pinax_close` closes the handles still open (calling their types' close functions). It does not free the
block; the block is the host's.]

#para("4")[The block serves three uses. Opening takes the fixed parts of the state: the banks, the call frames, the main
run's region, the tables of host words and handles. With the defaults that is about 150 KB, most of it the region
(64 KB) and the integer bank (32 KB), which `config` can shrink. The loaded program is stacked after it and stays.
While a load runs, the unused tail of the block is the compiler's working memory: about 20 KB for a small source,
about 1 KB more per word, and 64 KB more when the source has a constant pattern (16.7). The working memory is free
again when the load ends. A body uses working memory for the blocks it actually nests, not for the deepest nesting
allowed.]

== Host words <host-words>

#para("1")[`pinax_register(state, module, defs, count)` registers host functions as the words of one module. Each entry
of `defs` has a name, a signature text (in the form of 9.2, such as `"(s str gives n int or fail)"`), the function, a
user pointer and flags. Signatures are checked once, here. Register words before the sources that use them.]

#para("2")[A host function receives `(call, user, args)`. Arguments and results are in arrays by type: integers (and
`bool`, characters, handles and declared types over `int`) in `args->i[]` and `args->ri[]`, strings in `args->s[]` and
`args->rs[]`, in the order of the signature, counted from 0 within each type. The integer pointers point straight at the
state's slots. A narrow integer of the signature (7.6) travels as one 64-bit slot, and after the function returns the
processor checks the range of each result: outside it is the fault `overflow` (the result of a waiting word gets the
same check in `pinax_complete` and ends with that code). An argument of `pinax_run` outside its narrow type is
`PINAX_ERR_ARGUMENT`, and nothing runs.]

#para("3")[When the function returns something other than `PINAX_OK`, a word whose signature has `or fail` fails with
that code (11.2), and a word without it ends the run with a fault.]

#para("4")[A string result is copied into the caller's region unless the word has the flag `PINAX_WORD_KEEP_STR`. With
the flag it is not copied but pointed at; those bytes shall outlive the state and not change (static text, the
program's arguments and the like), and there is no limit to the number of such results. Room
for new bytes comes from `pinax_call_alloc`, in the caller's region. A parameter of type `buffer` in the signature is a
buffer of the script: `pinax_call_buffer` gives its free room and is told how many bytes were filled.]

#ex("spec", "19-host-words.c")

== Loading and lines <host-load>

#para("1")[`pinax_load(state, name, source, mode)` loads a source (5.2). `name` is the module's name; the modes are
`PINAX_LOAD_CODE` and `PINAX_LOAD_DATA` (13.4). A refusal is `PINAX_ERR_SYNTAX` or `PINAX_ERR_TYPE`, and the diagnostic
arrives at `config.diag` as (file, line, column, text).]

#para("1a")[`pinax_load_bytecode(state, bytes)` loads a bytecode container made by the processor into an empty state. The
shape of a container is the processor's (1 (3)). The processor does not trust a container; it checks it, and refuses
with `PINAX_ERR_FORMAT` one that could break 4 (2a). Among what this implementation refuses: registers outside the
window, jumps out of the word, a register read before it is written, a jump back or a call that does not charge the
budget, a tail call to a word with other results, an operation that can fail without a failure word after it or with
nothing after that, and an instruction that changes a string register and names it twice.]

#para("1b")[A load has no budget. So the processor loads a source or a container in time proportional to its length: no
pass of it, finding names, deciding which words yield or checking the flow of the registers, grows with the square
of the length. A host that takes untrusted sources therefore limits only their length, without fear that one load
never ends.]

#para("1c")[A load into a state that already holds a program appends what the new source adds to the program and
checks the new words only, so the bound of (1b) holds for the lines of a session and for modules loaded one by one.
The declaration tables are kept the same way: a load reads what earlier loads declared where it lies and appends
only its own declarations. The pass that decides which words yield covers the words the load brings, and the whole
program only when the load redefines a word. The room the program takes in the block grows with what was declared: when a part has no room
left to append to, the processor moves that part to a larger place, and when the load has ended it takes back the
room it left; while a task (clause 14) is alive it does not, and the next load after the task has ended does.
`redefine` appends the new body and leaves the old code where it was. The room used is read from `block_bytes_used`
of `pinax_stats`. A session line that only runs code takes no room.]

#para("2")[`pinax_eval(state, text)` runs one line as a session does. All lines form one module (the session). What a
line leaves is read with `pinax_eval_count` and `pinax_eval_value`.]

== Finding and running <host-run>

#para("1")[`pinax_find(state, name, signature, flags, &word)` finds a word and checks that its signature is the one
given; another is `PINAX_ERR_SIGNATURE`. A bare name is a word of the first source loaded; `module.word` a word of that
module (exported or not).]

#para("2")[`pinax_run(state, word, args)` runs a word. When the budget runs out it returns `PINAX_SUSPENDED`, and
`pinax_resume` goes on where it stopped; the budget does not change results. A fault returns its code, and the state
takes the next run.]

#para("3")[While a run is stopped (`PINAX_SUSPENDED`, `PINAX_WAITING`), a new `pinax_run` is `PINAX_ERR_STATE`. The
host either continues it to its end or gives it up with `pinax_abort`.]

#para("3a")[`pinax_abort(state)` ends a stopped run as a fault would: the handles the run opened are closed, its tasks
are freed and its pending tokens are forgotten (`pinax_complete` with one of them is `PINAX_ERR_NOT_FOUND`). What it
already stored in globals stays. When what stopped is the code of a load, the load is taken back as a failed one is.
The state then takes the next run. With nothing stopped it is `PINAX_ERR_STATE`. A host that must take the globals back
too saves an image (19.9) before the run.]

#ex("spec", "19-run.c")

#para("4")[`pinax_run` may be called from inside a host word too. There it runs only words found with the flag
`PINAX_FIND_NESTED`, and words that may yield (14.2) cannot be found with that flag.]

== Handles <host-handles>

#para("1")[`pinax_handle_type` makes a handle type with a name and a close function; scripts use the name as a type in
signatures. `pinax_handle_new` makes a handle of a C pointer, `pinax_handle_get` gives a handle's pointer (NULL for a
handle given back), and `pinax_handle_release` gives a handle back.]

#para("2")[A handle is a number and a generation (as in 8.9). A script cannot get an address from a handle. The handles
a run that ends with a fault opened are closed by the state, calling their close functions.]

#ex("spec", "19-handles.c")

== Binding <host-bind>

#para("1")[`pinax_bind(state, global, memory, bytes)` changes the storage of a global declared `bound` (8.12) to host
memory; the size shall match the declaration: for an array, the cells times the bytes of a cell (the width of a narrow
integer, 8 for any other value; 8.4). Script and host read and write the same bytes without copying.
`pinax_global_find` tells where a declared global is and how large it is.]

#ex("spec", "19-bind.c")

#para("1a")[`pinax_share(state, global, source, from)` gives a global that `state` declares `shared` (8.13) the
storage of the global `from` of `source`, without copying. Kind, type and capacity must be the same, else
`PINAX_ERR_SIGNATURE`. An index also needs the same hash key (19.2), and its table given first from the same `source`. A
list keeps the length it has then. When a table is given again, its index is given again too.]

#para("1b")[A `source` that lent storage is frozen: `pinax_run`, `pinax_load`, `pinax_eval` and `pinax_bind` on it are
`PINAX_ERR_STATE` from then on. So its bytes do not change, and states on several threads may read them at once.
`source` must stay open while the states that borrow from it use it. Until every shared global is given, runs are
refused with `PINAX_ERR_NOT_FOUND`. After an image is opened or a state cloned, give them again (19.9).]

#ex("spec", "19-share.c")

#para("1c")[Several processes share through a file. `pinax_share_save(source, out, &written)` writes the globals of
`source` that can be shared (scalars, arrays, lists, buffers and indexes; not those it declares `shared` itself) into
one file without addresses; `pinax_share_size` is its length. Saving copies, so `source` is not frozen. The host
library's `pinax_map_file(path, &bytes, &map)` maps the file read-only, so that the processes that map the same file
use the same pages. `pinax_share_file(state, global, bytes, from)` gives the file's global `from`, with the checks of
(1a); an index needs the hash key of the file and its table given first from the same file. The file's head, the size
and length of each global and the slots of an index are checked, else `PINAX_ERR_FORMAT`; its other bytes are trusted
as an image's are (19.9), except that no more than N bytes are read from a `str(N)` cell whose length byte says more.
`bytes` starts on a 16-byte boundary and outlives the states that read it: call
`pinax_unmap_file` after they are closed.]

#ex("spec", "19-share-file.c")

#para("1d")[While a run is not finished --- suspended by the budget, waiting for a host operation, or inside a host
word --- `pinax_bind`, `pinax_share` and `pinax_share_file` are `PINAX_ERR_STATE`: a view the run holds may point into
the storage they would replace.]

#para("2")[The `number` that `pinax_global_find` gives is what a word with a collection parameter (9.9) takes as that
argument, an integer in `i`. A number of a collection of another kind or another size of element, or of no global, is the
fault `PINAX_ERR_STATE` where the word uses it.]

== Host words that finish later <host-await>

#para("1")[A host word registered with `PINAX_WORD_AWAITABLE` may start some work, take a token with
`pinax_call_pending`, and return `PINAX_PENDING`. The task that called it waits and others run. When nothing else can
run, `pinax_run` or `pinax_resume` returns `PINAX_WAITING`. The host finishes the work with `pinax_complete(state,
token, outcome, values)` and calls `pinax_resume`.]

#ex("spec", "19-await.c")

#para("2")[In a state the host loop (17.4) runs, a host word takes its token with `pinax_loop_pending` in place of
`pinax_call_pending` and finishes the work with `pinax_loop_complete`, from any thread. The loop calls `pinax_complete`
on the state's thread and continues the run.]

== Images and clones <host-image>

#para("1")[An #term("image") is a whole state: code, declarations, globals, slots, regions, stopped runs and tasks.
`pinax_image_save` writes it and `pinax_image_open` opens it. An image that lies at the start of the block is opened in
place, without copying. An image of another format, or of another build's memory layout, is `PINAX_ERR_FORMAT`.]

#para("1a")[An image is data the host vouches for (4 (2a)): the processor checks its head, format and layout, and
that the addresses inside it lie in the block it was saved from, but not every byte. Open only images this host or
one it trusts saved. Strings that pointed outside the block (a host's text) and the last pattern match do not survive
an image.]

#para("2")[On an opened state the host gives host words, handle types and `extern` functions again, by name. Until all
are given, runs are refused with `PINAX_ERR_NOT_FOUND`. Handles come back closed (without their close functions), and
bound globals back on their own storage. `pinax_clone` saves and opens in another block in one call.]

#para("2a")[A clone opens in a block of `pinax_image_size(source)` bytes or more; a smaller block is
`PINAX_ERR_NO_ROOM`.]

#ex("spec", "19-image.c")

== Measuring, and the locals of a fault <host-measure>

#para("1")[`pinax_stats_get` gives the highest use of slots, region, handles and call depth, and the number of
instructions run. It is for choosing the block and the capacities.]

#para("2")[`pinax_fault_local` gives, after a run that ended with a fault, the locals of the word the fault happened in
with their values at that time (up to 16; text cut to its first 32 bytes, and shown only when its bytes lie in memory
the state can vouch for). They are what `pnx` shows below a fault
(11.3).]

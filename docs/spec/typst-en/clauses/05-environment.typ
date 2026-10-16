#import "/tools/common.typ": *
// shape: the refusal shown here is about loading as a whole; the rules are in later clauses

= Environment <environment>

== The state and memory <env-state>

#para("1")[A processor runs only inside the one block of bytes the host gave it. After a state is opened, the processor
asks the operating system for no memory. When the block is too small, the operation fails with `PINAX_ERR_NO_ROOM`;
nothing is written outside the block.]

#para("2")[Every value of a state is in one of these places: a declared global (clause 8), a slot of the stack or of a
local (clause 9), the region of a task (12.2), or memory the host gave and vouches for (bound and shared storage, 8.12, 8.13; strings
passed in; `KEEP_STR` results, 19.3). There is no other place. A value refers to another value by number (the number
of a row, a handle).]

#para("3")[A state uses no global or thread-local variables. Two states can therefore run at the same time on two
threads. One state is used by one thread at a time.]

== Loading <env-load>

#para("4")[A state takes any number of sources, one after the other. To *load* a source, the processor:
+ splits the source into tokens (clause 6) and reads its syntax;
+ reads it once from top to bottom, checking names, types, the depth of the stack, the handling of failures and the
  handing over of `owned` values;
+ when all is kept, runs once, in the order written, the code written outside definitions.]

#para("5")[The code outside definitions (#term("load code")) can use the words and globals of its source. The code
inside a word definition runs when the word is called.]

#ex("spec", "05-load-order.pnx")

#para("6")[When (4) 1 or 2 refuses, 3 does not happen. Not even load code written early in the source runs.]

#ex("spec", "05-refused-runs-nothing.pnx")

#para("7")[When load code stops with a fault, the load fails, and the words and globals that source added are gone.
Values that code stored into globals of sources loaded before, and what host words have already done, are not undone.]

#para("8")[There are two kinds of load. A #term("code load") accepts everything this document defines. A #term("data
load") accepts declarations and values only and refuses word definitions, calls and control (13.4). It is for text that
is not trusted, such as settings files.]

== Running <env-run>

#para("9")[The host finds a word by name and signature (clause 19), runs it with arguments and receives its results.
A run has a budget of instructions. When it is used up the run remembers where it stopped and returns to the host; when
the host continues it, it goes on from there. The budget does not change results.]

#para("10")[Below is a whole host program. It registers one word, loads a source and calls a word of the script.]

#ex("spec", "05-embed.c")

#para("11")[The command `pnx` is one host using this library. It loads a file, runs its load code and registers the
standard host modules (clause 17). Most examples in this document are run with `pnx` (clause 20).]

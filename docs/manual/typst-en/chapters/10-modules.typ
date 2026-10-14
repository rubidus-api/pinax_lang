#import "/tools/common.typ": *

= Modules and data files <modules>

#opening[Chapter 1 (`use`), chapter 3 (signatures).][
- How is a program split into several files?
- What does a module show, and what does it hide?
- How are settings read without running code?
]

== Making a module

One file is one module, named after the file without `.pnx`. What is to be seen outside is marked `export`.

#ex("manual", "money.pnx")

`cents` is a #strong[declared type] over `int`: represented as an integer, it does not mix with `int`. `raw` takes the
underlying integer out.

== Using a module

After `use money`, its words are written as paths such as `money.of_won`; the exported type `cents` is written by its
name alone. When `money` is not a registered module, `pnx` first loads `money.pnx` from beside the program.

#ex("manual", "10-use.pnx")
#ex("manual", "10-use-as.pnx")

`pad2`, not exported, is not seen outside.

#ex("manual", "10-private.pnx")

#mistake[`cents` is just an integer, so it can be added to one][Adding `cents` and `int` is refused. Only two values of
the same declared type are added and subtracted; the types are kept apart to stop numbers of different units from being
mixed.
#ex("manual", "10-mixed.pnx")]

== Data files

A host can read a source by #strong[data load]. A data load accepts declarations and values only, and refuses word
definitions, calls and control. So reading an untrusted settings file cannot run any code.

#ex("manual", "10-data.c")

`pnx` does not do data loads; they are the host's business (chapter 14).

#summary[
- One file is one module, and only what is `export`ed is seen outside (standard 13.1, 13.2).
- After `use MODULE [as ALIAS]` its parts are written `MODULE.NAME` (13.1).
- Values of a declared type are added and subtracted only with the same type (7.4).
- A data load accepts declarations and values only (13.4).
]

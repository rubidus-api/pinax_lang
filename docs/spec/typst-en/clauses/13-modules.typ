#import "/tools/common.typ": *

= Modules <modules>

== Using a module <modules-use>

#part("Syntax")
```ebnf
import = "use" NAME [ "as" NAME ] ;
```

#para("1")[One source is one *module*. The name the host gave when loading the source is the module's name (`pnx` uses
the file name without `.pnx`). Standard modules and host modules (13.3) are modules too.]

#para("2")[`use MODULE` opens that module in this source; from then on its parts are written as the *path*
`MODULE.NAME`. `use MODULE as ALIAS` makes the same module be written `ALIAS.NAME`. Only core words and what this source
declares are written as bare names, and the words of the modules a search order names (13.5).]

#para("3")[A module that is used shall exist already: a standard module, a module the host registered, or a source
loaded earlier. When `pnx` meets `use NAME` it first loads `NAME.pnx` from beside the program file.]

#ex("spec", "13-use.pnx")
#ex("spec", "13-use-as.pnx")

#part("Constraints")

#para("4")[Using a module that does not exist is refused.]
#ex("spec", "13-unknown-module.pnx")

#para("5")[A word of a module written as a bare name is refused, unless a search order names that module (13.5); so is
a path into a module that was not opened.]
#ex("spec", "13-bare-name.pnx")
#ex("spec", "13-no-use.pnx")

== Exports <modules-export>

#para("1")[Another source can use only what was declared or defined with `export`. Words, constants and globals are
written `MODULE.NAME` (an exported global is stored with `set MODULE.NAME`); exported `record`s and `type`s and the row types of tables are written by their name alone.]

#para("2")[The module `geometry` below is the one the examples of 13.1 use. `square` is not exported.]

#ex("spec", "geometry.pnx")

#part("Constraints")

#para("3")[Using what is not exported is refused.]
#ex("spec", "13-not-exported.pnx")

== Standard modules and host modules <modules-kinds>

#para("1")[The #term("standard module")s come with the processor and are part of the language: `str` `fmt` `hash`
`encode` `bytes` `ptn` `rex` `json` `csv` (clause 16). They are seen only after `use`.]

#para("2")[A #term("host module") is a set of words the host registered. A module the host did not register does not
exist. Everything a script can do outside itself (files, clocks, the environment) is a word the host registered. `pnx`
registers the standard host modules `io` `file` `clock` `random` `env` (clause 17).]

#para("3")[`err` is a set of constants written as paths without `use` (11.1).]

== Data load <modules-data>

#part("Syntax")
```ebnf
data-source = { data-item } ;
data-item   = [ "export" ] ( [ "bound" ] bindable | "const" NAME literal
              | "list" NAME type size | "queue" NAME type size | "pool" NAME NAME size
              | "record" NAME { field | "big_endian" } "end" | "type" NAME base
              | "index" NAME NAME PATH )
            | "forward" ( "array" | "list" | "pool" ) NAME
            | row-item | assign-item
            | data-value [ data-value ] "set" NAME              (* a global, or a cell *)
            | data-value NAME "to" PATH ;                      (* a field of a named row *)
data-value  = literal | NAME ;                      (* NAME: a const or a row name *)
```

#para("1")[A source the host reads by data load accepts declarations and values only. Word definitions, calls, `use`,
`extern`, `test`, control, blocks and `cat` are refused. Such a source therefore cannot run any code. It is for reading
settings, data and other text that is not trusted.]

#para("2")[A value is stored with `value set NAME`, a cell with `value index set NAME` (`"10.0.0.1" 0 set allow`) and
a field of a named row with `value ROW to RECORD.FIELD`, as in code (8.8, 9.4).
Everything a data source declares counts as exported.]

#ex("spec", "13-data-load.c")

#part("Diagnostics")
#diag(
  ([A module that does not exist], "no module of this name is registered or loaded"),
  ([A path into a module not opened, or no record], "this path names neither a module in use nor a record"),
  ([A name not exported], "the module exports nothing of this name"),
  ([A definition or `use` in a data load], "a data source defines no words and uses no modules"),
  ([A call in a data load], "a data source calls no words; only literals, const names and stores"),
)

== The search order <modules-search>

#part("Syntax")
```ebnf
order     = "set_order" "(" [ order-one { "," order-one } ] ")" | "get_order" ;
order-one = NAME | "get_order" ;
```

#para("1")[`set_order (io, str)` gives the #term("search order"): the modules in which a bare name is looked for. Each
module named is opened as by `use` (13.1 (2), (3)); `pnx` loads a file module beside the program first, as for `use`.
`set_order ()` empties the order. Without a search order, nothing is looked for in a module.]

#para("1a")[Inside the parentheses, `get_order` stands for the search order as it is at that place: `set_order (json,
get_order)` puts `json` in front of it, `set_order (get_order, csv)` behind it. A module that `get_order` brings again,
or that is named again after `get_order` brought it, keeps its first place.]

#para("2")[A bare name in code is looked for in this order, and the first that has it is taken: the locals and
parameters; what this source (or session) declares; the core words; then the modules of the search order, left to
right. So a name of the source and a core word keep their meaning whatever the order holds, and a module added to it
cannot change code that compiled before. A word of a module named like a core word (`str.len` beside `len`) is written
with its path. A path is read as before.]

#ex("spec", "13-order.pnx")
#ex("spec", "13-order-core.pnx")

#para("3")[`set_order` stands at the top level of a source or of a session line, as often as needed. Each one
replaces the order from there to the end of the source; a session keeps it for the lines after, until the next one.
A word takes what its names found when it was compiled.]

#para("3a")[`get_order` alone at the top level tells the search order at that place, written as `set_order` takes
it, `order: (io, str)`, as a diagnostic with line 0 at `config.diag` (19.4 (1)), and changes nothing. It speaks when the source is checked: in a
file, before any of the file's code runs.]

#para("4")[A name reached through the search order does not count for the rule against hiding names (9.5): a local
may have the name of a word of a module in the order, and inside that word the local is meant.]

#part("Constraints")

#para("5")[`set_order` or `get_order` inside a definition or a block, a search order that names a module twice or more
than 16 modules, and one that names a module that does not exist are refused.]

#part("Diagnostics")
#diag(
  ([`set_order` inside a definition], "set_order stands at the top level, outside definitions and blocks"),
  ([`get_order` inside a definition], "get_order stands at the top level, or inside set_order (...)"),
  ([No parentheses], "set_order is followed by modules in parentheses: set_order (io, str)"),
  ([Not a module name], "set_order names modules, separated by commas: set_order (io, str)"),
  ([A module named twice], "this module is named twice in the order"),
  ([More than 16], "an order names at most 16 modules"),
  ([`search (…)`, not a word of the language], "search is now set_order: set_order (io, str)"),
)

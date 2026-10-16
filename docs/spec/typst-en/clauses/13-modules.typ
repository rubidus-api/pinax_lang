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
declares are written as bare names.]

#para("3")[A module that is used shall exist already: a standard module, a module the host registered, or a source
loaded earlier. When `pnx` meets `use NAME` it first loads `NAME.pnx` from beside the program file.]

#ex("spec", "13-use.pnx")
#ex("spec", "13-use-as.pnx")

#part("Constraints")

#para("4")[Using a module that does not exist is refused.]
#ex("spec", "13-unknown-module.pnx")

#para("5")[A word of a module written as a bare name is refused; so is a path into a module that was not opened.]
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

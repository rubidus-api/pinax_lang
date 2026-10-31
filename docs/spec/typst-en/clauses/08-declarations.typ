#import "/tools/common.typ": *

= Declarations <declarations>

== General <decl-general>

#part("Syntax")
```ebnf
declaration = [ "export" ] ( [ "bound" ] bindable | "shared" shareable | decl ) ;
bindable    = "global" NAME type
            | "array"  NAME type capacity
            | "buffer" NAME capacity ;
shareable   = bindable
            | "list"   NAME type capacity
            | "index"  NAME NAME PATH ;
decl        = "const"  NAME literal
            | "list"   NAME type capacity
            | "queue"  NAME type capacity
            | "pool"   NAME NAME capacity
            | "record" NAME { field | "big_endian" } "end"
            | "type"   NAME base
            | "index"  NAME NAME PATH
            | "tasks"  NAME capacity [ size [ size [ size ] ] ] [ "apart" ]
            | "extern" c-name "(" effect ")" "from" STRING [ "as" NAME ]
            | "test"   NAME { body } "end" ;
capacity    = size | "host" ;
size        = INT | NAME ;                         (* NAME: a const *)
```

#para("1")[A #term("declaration") makes a name and what it stands for (a value, storage, a type). Declarations are
written only outside definitions, at the outermost level of a source.]

#para("2")[A processor reads a source once, from top to bottom. A name shall be declared before the place where it is
used. The exceptions are a word that calls itself, and `forward` (8.11).]

#para("3")[Within one module a name cannot be made twice, not even for different kinds of things. The names of core
words and types cannot be made again either.]

#para("4")[A capacity (`size`) is a positive integer literal or a `const` of one. Capacities are fixed at load and do
not change while running. All storage is placed inside the state's block at load.]

#para("4a")[The capacity of an array, a buffer, a list, a queue, a pool or a task group may be written `host`. Then it
is the number the host gave for that declaration before the load (19.4 (1d)), and the load treats it as that number
written there. A load that meets `host` for a declaration the host gave no number is refused, and the message names
the declaration. `host` is a word of the declaration only in that place; a constant of that name is not looked up
there.]
#ex("spec", "08-host-capacity.pnx")

#para("5")[Globals, cells of arrays, slots of lists, queues and pools, buffers and fields of records start at zero
(7 (3)).]

#part("Constraints")

#para("6")[Making a name that exists already is refused.]
#ex("spec", "08-name-twice.pnx")

#para("7")[Using a name before its declaration is refused.]
#ex("spec", "08-use-before.pnx")

== Constants <decl-const>

#para("1")[`const NAME literal` makes a #term("constant"). Its type is the literal's. A constant has no storage: its
value goes where it is used. Constants can be capacities, `case` values and `row` values.]

#part("Constraints")

#para("2")[Nothing can be stored into a constant. Inside a definition `v to CONST` would make a new local of the same
name and is refused by the rule against hiding names (9.5); outside a definition `to` makes no local, so it is
refused. `set` writes globals only (9.4).]
#ex("spec", "08-const-store.pnx")

== Globals <decl-global>

#para("1")[`global NAME type` makes a #term("global") that holds one value. The type is `int`, `bool`, `err`,
`dec(N)`, `str(N)`, a declared type, a row type (the name of a table) or a handle type; not `str` (7.3).]

#para("2")[A global is read by writing its name and stored with `v set NAME` (9.4). Its value stays as long as the state does
--- after a word's run ends, and across later loads.]

#ex("spec", "08-const-global.pnx")

== Arrays <decl-array>

#part("Syntax")
```ebnf
assign-item = "assign" NAME "(" value { "," value } ")" ;
```

#para("1")[`array NAME type capacity` makes an #term("array") with that many cells. An array is always full: `len` is
its capacity. The element type is `int`, `bool`, `err`, `dec(N)`, `str(N)`, a narrow integer (7.6), a declared type,
the name of a record, or the name of a table, whose rows the cells then hold (8.8 (5a)). A cell of a narrow integer takes the bytes of its width, a cell of any other value 8 bytes; the
cells of lists and queues are the same.]

#para("2")[`i ARRAY idx` reads cell `i`; `v i set ARRAY` stores into cell `i`; a cell of a collection parameter (9.9) is stored the same way. Numbers count from 0. For an array of
records `idx` gives a row (8.8), not a value.]

#para("3")[`assign ARRAY (value, …)` fills an array from cell 0, outside definitions. With fewer values than cells the
rest stay as they are. Assigning the same array again starts from cell 0 again. It may be used in a data load (13.4).]

#para("4")[The name of an array used as a stem yields its cells from 0 in order (10.6).]

#ex("spec", "08-array.pnx")

#part("Constraints")

#para("5")[More values in `assign` than the array has cells is refused.]
#ex("spec", "08-assign-many.pnx")

#part("Faults")

#para("6")[An index below 0, or equal to or above the capacity, is the fault `range`.]
#ex("spec", "08-array-range.pnx")

== Lists <decl-list>

#para("1")[`list NAME type capacity` makes a #term("list") whose length grows from 0 up to its capacity. Its cells are
read and stored like an array's, but only indexes below the length may be used.]

#para("2")[`v LIST push` adds one value at the end; `LIST pop` takes the last one off and gives it. Both can fail
(`or fail`): a `push` into a full list fails with `err.full`, a `pop` from an empty list with `err.range`; a `push` of
text too long for a list of `str(N)` fails with `err.range` (7.3 (4a)). A list of
records gets an empty row at the end with `LIST add_row`, which gives that row. `LIST clear` sets the length to 0.]

#ex("spec", "08-list.pnx")
#ex("spec", "08-list-full.pnx")

#para("2a")[`n LIST truncate` cuts the length to `n`, for lists of integers, of text and of records alike. It takes back
a row added with `add_row` whose values turn out to be wrong.]

#ex("spec", "08-list-truncate.pnx")

#part("Constraints")

#para("2b")[A list of records whose rows are kept elsewhere cannot be truncated. It is refused when the same source has
a field, a global or a collection that holds its rows, a row of it named with `row`, or an index of it (8.10): once a new row takes
the place cut off, they would point at the wrong row. A row held in a local is checked when it is used (fault
`range`).]

#ex("spec", "08-list-truncate-kept.pnx")

#part("Faults")

#para("3")[An index past the length is the fault `range`, even when it is below the capacity. So is a number for
`truncate` larger than the length, or below zero.]
#ex("spec", "08-list-past-len.pnx")

#part("Diagnostics")
#diag(
  ([`truncate` on a list whose rows are kept], "this list's rows are kept by a field, a global, a collection, a named row or an index; it cannot be truncated"),
)

== Queues <decl-queue>

#para("1")[`queue NAME type capacity` makes a #term("queue"): what goes in first comes out first. The element type is
`int`, `bool`, `err` or `dec(N)`. `v QUEUE put` puts a value at the back, `QUEUE take` takes the front one off and gives it, and
`QUEUE peek` gives the front one without taking it. All three can fail: putting into a full queue is `err.full`, taking
from an empty one `err.range`.]

#ex("spec", "08-queue.pnx")

== Buffers <decl-buffer>

#para("1")[`buffer NAME capacity` makes a #term("buffer") of that many bytes: a sequence of bytes that can be changed,
for building text piece by piece.]

#para("2")[`s BUF append`, `n BUF append_int` (decimal; `n` an `int` or a narrow integer) and `b BUF append_byte` (`b` a
`u8`) add at the end; when there is not
enough room they fail with `err.full` and add nothing. `BUF contents` gives a `str` view of what it holds now.
`n BUF truncate` cuts the length to `n`. `BUF clear` sets the length to 0.]

#ex("spec", "08-buffer.pnx")

#para("3")[The view `contents` gives points at the buffer's bytes: when the buffer changes, so do the bytes the view
sees.]

== Records and rows <decl-record>

#part("Syntax")
```ebnf
record   = "record" NAME { field | "big_endian" } "end" ;
field    = NAME width ;
width    = "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64"
         | "int" | "bool" | "varint" | "dec" "(" INT ")" | "str" "(" size ")"
         | NAME ;                                         (* NAME: a table *)
row-item = [ "export" ] "row" NAME [ NAME ] "(" value { "," value } ")" ;
value    = literal | NAME ;                         (* NAME: a const, a row name, none *)
```

#para("1")[`record NAME … end` makes a #term("record") type, a sequence of fields, each with a width. An `int` or `i64` field
reads as `int`, a `u8` … `i32` field as that narrow integer (7.6), a `bool` field as `bool`, a `dec(N)` field as
`dec(N)` (8 bytes), a `str(N)` field as a `str` view. A field takes a value of its type only. A field whose type is the name of a table
holds a row of that table.]

#para("2")[Records are not handled as values. One declares an array, list or pool of records (a #term("table")) and
handles its #term("row")s. A row's type is named after its table. A row holds a number within the table, and every use
checks its range (and, for a pool, its generation, 8.9).]

#para("3")[Fields are written as the path `RECORD.FIELD`: `row RECORD.FIELD` reads, `v row to RECORD.FIELD` stores.]

#para("4")[`row TABLE (value, …)` adds one row to a list, outside definitions. Values go in field order; trailing
fields may be left out (they are zero). `row TABLE NAME (…)` names the row; the name stands for the row in later `row`
values and in code.]

#ex("spec", "08-record.pnx")

#para("5")[A field or global that holds a row starts as "none". `row is_none` asks whether it is none, `TABLE none`
gives the none row of that table, and `row row_index` gives the row's number.]

#para("5a")[An array or a list whose element type is the name of a table holds rows of that table, in cells of 8 bytes:
the row's number plus one, with a pool row's generation in the upper 32 bits; 0 is none, and every cell starts as none.
Its cells are read, stored, pushed, popped and walked as any cells are, and a value going in shall be a row of that
table. A row read from a cell is checked where it is used, as any row is. A queue holds no rows. A row that `alloc` has
just given (8.9) is stored into a cell with `set`, which takes it over as a field does; `push` refuses it, since a
`push` that fails would lose it.]

#ex("spec", "08-row-cells.pnx")

#para("6")[`big_endian` sets only the byte order that `bytes.pack` and `bytes.unpack` of 16.5 use. It does not change
the layout in memory.]

#part("Constraints")

#para("7")[The values of `row` shall match the fields in order and type.]
#ex("spec", "08-row-types.pnx")

#part("Faults")

#para("8")[An `int` stored into a narrow integer field is converted first (`n u8`); a value that does not fit the width
makes that conversion the fault `overflow` (7.6). Nothing is cut: to store the low bits take a word such as `low_u8`.
Reading or storing a field of the none row is the fault `range`.]
#ex("spec", "08-record-narrow.pnx")

== Pools <decl-pool>

#para("1")[`pool NAME RECORD capacity` makes a #term("pool") of rows that are taken and given back. `POOL alloc` takes
a free row (`or fail`, `err.full` when none is left); `row POOL free` gives it back; `row POOL is_alive` asks whether
the row is still alive.]

#para("2")[A row of a pool holds a #term("generation") besides its number. Giving a row back changes the generation of
its slot, so a row used after it was given back is noticed --- even when another row has taken the slot since.]

#para("3")[The row `alloc` gives is an `owned` value (12.4). On every path it shall be stored exactly once into a field
or global, or given to `free`. Once stored, the generation check guards it.]

#ex("spec", "08-pool.pnx")

#part("Constraints")

#para("4")[A path on which a taken row is neither stored nor given back is refused.]
#ex("spec", "08-pool-leak.pnx")

#part("Faults")

#para("5")[Using a row that was given back is the fault `stale`. `pnx` shows the locals of the word below the fault.]
#ex("spec", "08-pool-stale.pnx")

== Indexes <decl-index>

#para("1")[`index NAME TABLE RECORD.FIELD` makes an #term("index") that finds rows of a table of records by one field.
The key field is `int` or `str(N)`. An index is a hash table of fixed size, matched to the table's capacity, that holds
only row numbers; the data stay in the table.]

#para("2")[`key INDEX lookup` gives the row with that key (`or fail`, `err.not_found` when there is none).
`row INDEX insert` puts a row in (`or fail`; it fails when the key is there already) and `row INDEX remove` takes it
out. To change a key field, remove the row, change it and insert it again.]

#para("3")[Rows written with `row` at load are in the index. Rows added while running with `add_row` are found only
after `insert`.]

#ex("spec", "08-index.pnx")
#ex("spec", "08-index-insert.pnx")

#para("4")[The hash is keyed (SipHash-2-4); the host gives the key (19.2). A host that indexes text from outside should
give a random key: whoever knows the key can pile up collisions and make lookups slow.]

== Forward declarations <decl-forward>

#part("Syntax")
```ebnf
forward = "forward" ( "array" | "list" | "pool" ) NAME
        | "forward" "word" NAME "(" effect ")" ;
```

#para("1")[`forward list NAME` announces a table declared further on. It is for a record that holds rows of the table
that holds it. The name of an announced table may be used only where a type goes. `forward word` is in 9.7.]

#ex("spec", "08-forward.pnx")

#part("Constraints")

#para("2")[An announced table shall be declared in the same module with the same kind.]
#ex("spec", "08-forward-missing.pnx")

== Bound storage <decl-bound>

#para("1")[`global`, `array` and `buffer` with `bound` in front get their storage from the host. Scripts use them like
other globals. Until the host binds them they use room inside the state. 19.7 says how to bind.]

== Shared storage <decl-shared>

#para("1")[`global`, `array`, `list`, `buffer` and `index` with `shared` in front only read another state's storage.
The declaring state has no storage for them; the host gives it the same declaration of another state with
`pinax_share` (19.7). Many states may read the same data at once, on many threads, with no copy and no lock.]

#part("Constraints")

#para("2")[Nothing changes a shared global: storing, `set`, `push`, `pop`, `clear`, `truncate`, `add_row`, storing
into a field, `insert`, `remove`, the words that append to a buffer, and a `map_into` or `filter_into` destination are
all refused. The processor also refuses, at load, bytecode that changes a shared global, and ends with the fault
`state` bytecode that would change a shared global it was given as a parameter.]
#ex("spec", "08-shared-write.pnx")

#para("3")[A shared collection may be passed as a collection parameter (9.9) of a word that only reads that parameter.
A word changes its parameter when it uses an instruction that changes it, or passes it to a word that changes that
parameter or to a host word's `buffer` parameter. The processor decides once the whole source is read, so the same
holds for a word announced with `forward` and for a word of an earlier load.]
#ex("spec", "08-shared-param.pnx")
#ex("spec", "08-shared-change.pnx")

#para("3a")[A shared collection is not passed as a host word's `buffer` parameter or as a parameter of the word a block
value names (`call`), since what the receiver does is not known at load. `redefine` cannot make a word change a
parameter it only read.]

#para("4")[`shared` does not stand before `queue`, `pool`, `tasks`, `const` or `record`.]
#ex("spec", "08-shared-queue.pnx")

#part("Semantics")

#para("5")[Until the host gives it, it is empty: lists and arrays have length 0, an index finds nothing, a scalar is 0
and a buffer has no bytes. The code of a load runs as usual. Runs of the host are refused until every shared global is
given (19.7).]

#part("Diagnostics")
#diag(
  ([A name that exists already], "this name is already defined"),
  ([A name used before its declaration], "this name is not declared before this place"),
  ([A capacity `host` the host gave no number for (the declaration's name in the message)], "the host gave no capacity for NAME (pinax_capacity)"),
  ([`to` outside a definition into a name that is not a global], "outside a definition `to` makes no new local; a global is written with set"),
  ([More `assign` values than cells], "assign has more values than the array has cells"),
  ([A queue of another element type than `int`, `bool`, `err`, `dec(N)`], "a queue holds int, bool, err or dec(N) values"),
  ([`push` of a row `alloc` has just given (8.8 (5a))], "push can fail and lose an owned row; store it with set, into a field or into a global"),
  ([A `row` value unlike its field], "a value has another type than its field"),
  ([A path that does not hand over an `owned` row], "an owned value in a local is not given on every path (close, free or store it)"),
  ([An announced table that is never declared], "a table announced by forward is never declared"),
  ([A shared global is changed], "a shared global is read-only: nothing here may change it"),
  ([A shared collection passed to a word that changes it], "a shared collection is read-only: the word it is given to changes this parameter"),
  ([A shared collection passed to a block value's word], "a shared collection is read-only: it is not given to the word a block value names, which may change it"),
  ([`redefine` makes a read parameter changed], "redefine cannot make the word change a collection parameter the old one only read: shared collections may have been given to it"),
  ([A shared buffer passed to a host word], "a shared buffer is read-only: it is not given to a host word, which may write it"),
  ([Another declaration after `shared`], "shared stands before global, array, list, buffer or index"),
)

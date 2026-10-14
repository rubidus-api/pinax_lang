#import "/tools/common.typ": *

= Types and values <types>

#para("1")[Every value has a type fixed at load. Values do not carry their type while running, and no word asks for a
type while running. A program whose types do not match is refused at load.]

#para("2")[The types are:
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Type*], [*Values*],
  [`int`], [64-bit two's complement integers (7.1)],
  [`bool`], [`true`, `false` (7.2)],
  [`str`], [a view of bytes (7.3); only in locals, arguments and results],
  [`str(N)`], [stored text of up to N bytes (7.3); in globals, cells of arrays, fields of records],
  [`err`], [the error code of a failure (11.1)],
  [declared type], [a type made with `type NAME base`: the same representation as its base, a different type (7.4)],
  [row], [one row of a table of records (array, list, pool); the type is named after the table (8.8)],
  [handle], [a number that refers to an object the host gave; the host names the type (19.6)],
  [block], [a word value, written `word (signature)` (10.5)],
  [`regex`], [a regular expression program (16.7); visible only in a source that has `use rex`],
  [`buffer`], [a parameter type in host word signatures only; it takes a buffer the script declared (8.7, 19.3)],
)]

#para("3")[Globals, cells of arrays and fields of records start at zero: `int` 0, `bool` `false`, `str(N)` the empty
text, a row "none" (8.8).]

== Integers <types-int>

#para("1")[The values of `int` are the integers from −2#super[63] to 2#super[63]−1, the same on every processor.]

#para("2")[The arithmetic words `add` `sub` `mul` `div` `mod` `neg` `abs` stop with the fault `overflow` when the
result is out of range (11.3). There are words that wrap (`wrap_add` …), that stop at the edge (`sat_add` …), and that
report overflow as a failure (`chk_add` …) (15.1).]

#para("3")[`div` rounds toward zero, and the result of `mod` has the sign of the dividend (`a b div b mul a b mod add`
is `a`). `floor_div` and `floor_mod` round down. Division by zero is the fault `div_zero`.]

#ex("spec", "07-int.pnx")

#part("Faults")

#para("4")[An overflow ends the run on the spot. In the example below the code after the line that overflows does not
run.]
#ex("spec", "07-int-overflow.pnx")

== Booleans <types-bool>

#para("1")[`bool` has two values, `true` and `false`. Comparison words (`eq` `lt` …) give them; `if`, `while`, `until`
and the logical words (`and` `or` `not`) take them.]

#ex("spec", "07-bool.pnx")

#part("Constraints")

#para("2")[`bool` and `int` do not convert into each other. An `int` cannot stand where a condition goes, nor a `bool`
where arithmetic goes.]
#ex("spec", "07-bool-not-int.pnx")
#ex("spec", "07-int-not-bool.pnx")

== Strings <types-str>

#para("1")[A #term("string") is a sequence of bytes. There is no rule that it be UTF-8: words that work on bytes read
bytes, and words that work on characters (`str.char_count` and so on) read UTF-8.]

#para("2")[There are two string types.
- `str` is a *view*: the start and length of bytes that are somewhere (a literal, the text of a global, a buffer, the
  region). Making one copies no bytes. It lives only in locals, arguments, results and on the stack.
- `str(N)` is #term("stored text"). It takes N bytes of room and copies the bytes put into it. It is used only in
  globals, cells of arrays and lists, and fields of records. Reading it gives a `str` view of that room.]

#ex("spec", "07-str.pnx")

#part("Constraints")

#para("3")[`str` for a global, an array or a field of a record is refused: the bytes a view points at may disappear
before that place does.]
#ex("spec", "07-str-global.pnx")

#part("Faults")

#para("4")[Putting text longer than N bytes into a `str(N)` place stops with the fault `range`. It is not cut.]
#ex("spec", "07-str-too-long.pnx")

#para("5")[A view of bytes made in the region cannot be used once the region is rewound past them; 12.3 states this
rule.]

== Declared types <types-declared>

#part("Syntax")
```ebnf
type-decl = "type" NAME base ;
base      = "int" | "bool" | "str" ;
```

#para("1")[`type meters int` makes the #term("declared type") `meters`. Its values are represented like `int` and cost
nothing more to run, but at load it is a different type. It keeps numbers of different units from being mixed.]

#para("2")[The name of the type written after a value of the base type makes a value of that type (`1500 meters`). The
word `raw` turns a value of a declared type back into its base type.]

#para("3")[Two values of the same declared type can be added and subtracted with `add` and `sub`; the result has that
type. Comparison words also take two values of the same type. Other arithmetic is done after `raw`.]

#ex("spec", "07-type.pnx")

#part("Constraints")

#para("4")[A value of a declared type given where another type goes is refused. Below, the two arguments are swapped.]
#ex("spec", "07-type-mixed.pnx")

#para("5")[Arithmetic words other than `add` and `sub` on two values of a declared type are refused.]
#ex("spec", "07-type-arith.pnx")

#part("Diagnostics")
#diag(
  ([A value that is not a `bool` where a condition goes], "a bool is expected here"),
  ([The two values of an arithmetic word differ in type or are not `int`], "this word takes two int values, or two of the same declared type"),
  ([Arithmetic other than `add` `sub` on a declared type], "values of a declared type are added and subtracted; for other arithmetic take raw"),
  ([An argument of another type than the signature], "an argument has another type than the word's signature asks for"),
  ([`str` for a global and the like], "stored text is str(N); a str view lives only in locals, arguments and results"),
)

#import "/tools/common.typ": *

= Types and values <types>

#para("1")[Every value has a type fixed at load. Values do not carry their type while running, and no word asks for a
type while running. A program whose types do not match is refused at load.]

#para("2")[The types are:
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Type*], [*Values*],
  [`int`], [64-bit two's complement integers (7.1)],
  [`dec(N)`], [decimals with N digits after the point, N from 0 to 18 (7.5)],
  [`u8` `u16` `u32` `u64` `i8` `i16` `i32`], [unsigned and signed integers of that width (7.6); `u8` is a byte, `u32` a code point],
  [`bool`], [`true`, `false` (7.2)],
  [`str`], [a view of bytes (7.3); only in locals, arguments and results],
  [`str(N)`], [stored text of up to N bytes (7.3); in globals, cells of arrays, fields of records],
  [`err`], [the error code of a failure (11.1)],
  [declared type], [a type made with `type NAME base`: the same representation as its base, a different type (7.4)],
  [row], [one row of a table of records (array, list, pool); the type is named after the table (8.8)],
  [handle], [a number that refers to an object the host gave; the host names the type (19.6)],
  [block], [a word value, written `word (signature)` (10.5)],
  [`regex`], [a regular expression program (16.7); visible only in a source that has `use rex`],
  [`buffer`], [a parameter type: it takes a buffer the script declared (8.7, 9.9, 19.3)],
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
base      = "int" | "bool" | "str" | "dec" "(" INT ")" ;
```

#para("1")[`type meters int` makes the #term("declared type") `meters`. Its values are represented like `int` and cost
nothing more to run, but at load it is a different type. It keeps numbers of different units from being mixed.]

#para("2")[The name of the type written after a value of the base type makes a value of that type (`1500 meters`). The
word `raw` turns a value of a declared type back into its base type.]

#para("3")[Two values of the same declared type can be added and subtracted with `add` and `sub`; the result has that
type. Comparison words and `min` `max` also take two values of the same type. Other arithmetic is done after `raw`.]

#ex("spec", "07-type.pnx")

#para("3a")[A value of a declared type `D` over `int` multiplied by an `int` (`D int mul`, `int D mul`), divided by
one or taken modulo one (`D int div`, `D int mod`) gives a `D`. `neg` and `abs` give a `D` too. Scaling a number up or
down keeps its unit. The product and the quotient of two `D` values are not a `D`, so this rule does not cover them.]

#ex("spec", "07-type-scale.pnx")

#part("Constraints")

#para("4")[A value of a declared type given where another type goes is refused. Below, the two arguments are swapped.]
#ex("spec", "07-type-mixed.pnx")

#para("5")[Arithmetic words other than `add` and `sub` on two values of a declared type are refused. So is any
arithmetic word outside (3a) on a declared type and an `int`.]
#ex("spec", "07-type-arith.pnx")

== Decimals <types-dec>

#part("Syntax")
```ebnf
dec-type = "dec" "(" INT ")" ;   (* 0 to 18 *)
DEC      = [ sign ] digits "." digits ;   (* no exponent: 6.4 *)
```

#para("1")[A value of `dec(N)` is a decimal number with `N` digits after the point. It is held as an `int` scaled by
10#super[N]: `12.50` is a `dec(2)` held as 1250. Results are the same on every processor. The range is that of `int`
divided by 10#super[N].]

#para("2")[A number with a point and digits on both sides, without an exponent, is a `dec(N)` literal whose `N` is the
count of digits after the point: `12.50` is `dec(2)`, `0.5` is `dec(1)`, `-0.05` is `dec(2)`.]

#para("3")[`dec(N)` is a number type over `int` as a declared type is (7.4): two values of the same `dec(N)` are added,
subtracted, compared and given to `min` and `max`; a `dec(N)` is multiplied by an `int`, divided by one (rounded toward
zero, as `div`) and taken modulo one; `neg` and `abs` keep it. In addition `dec(A) dec(B) mul` gives `dec(A+B)`, exact,
when `A+B` is at most 18. `sum`, `minimum` and `maximum` (10.6) take stems of `dec(N)` and give one. `chk_add`
`chk_sub` `chk_mul` `chk_div` take a `dec(N)` as `add` `sub` `mul` `div` do. An overflow is the fault `overflow`.]

#para("4")[`n dec(N)` makes the `int` `n` into `n` with `N` zeros after the point (`5 dec(2)` is `5.00`); `d raw` gives
the scaled `int` (`12.50 raw` is 1250). `d M rescale` gives `d` as a `dec(M)`, `M` a constant from 0 to 18: more digits
are exact; fewer are rounded half away from zero (`2.345` is `2.35`, `-2.345` is `-2.35`).]

#ex("spec", "07-dec.pnx")
#ex("spec", "07-dec-round.pnx")

#para("5")[`type price dec(2)` declares a type over `dec(2)`; `9.99 price` makes one and `raw` gives the `dec(2)`.
`dec(N)` is the type of globals, fields (8 bytes), cells of arrays and lists, values of queues and parameters (9.9) as
`int` is. `str.to_str` and `cat` write it with all its digits (16.1); `str.to_dec` reads one.]

#part("Constraints")

#para("6")[Two `dec` values of different scales are not added or compared; `rescale` one first. A `dec` is not divided by
a `dec`.]
#ex("spec", "07-dec-scales.pnx")
#ex("spec", "07-dec-div.pnx")

== Narrow integers <types-narrow>

#para("1")[`u8` `u16` `u32` `u64` `i8` `i16` `i32` are the #term("narrow integer", en: "narrow integer") types. The name
says the range: `u` from 0 to 2#super[width]−1, `i` from −2#super[width−1] to 2#super[width−1]−1. `u8` is the type of a
byte and `u32` the type of a code point. The 64-bit signed integer is `int`; there is no type `i64`.]

#para("2")[A narrow integer is another type than `int`, and each is another type than the others. Conversion is a word:
`n u8` makes the `int` or other narrow integer `n` into a `u8`, and a value outside the range is the fault `overflow`.
Nothing is truncated. A literal is written the same way: `255 u8`. A constant outside the range is refused at load.
`x raw` gives the `int` held; `raw` of a `u64` puts its 64 bits into the `int` as they are (2#super[63] and above are
negative). `n low_u8` … `n low_i32` take the low bits only: these are the truncating words.]

#para("3")[`add` `sub` `mul` `div` `mod` take two of one narrow integer type and give that type; a result outside the
range is the fault `overflow`. `wrap_add` `wrap_sub` `wrap_mul` wrap modulo 2#super[width], `sat_add` `sat_sub`
`sat_mul` stop at the ends of the range, and `chk_add` `chk_sub` `chk_mul` `chk_div` fail. The comparison words take two
of one type; `u64` compares and divides unsigned. `neg` `abs` `min` `max` `clamp` do not take them (15.1).]

#para("4")[The bit words work within the width. `bit_not` flips the bits of the width only, `shl` drops the bits that
leave the width, and `shr` keeps the sign of a signed type and fills an unsigned one with zeros. `u_shr` `u_div` `u_mod`
`u_lt` `u_le` `u_gt` `u_ge` `floor_div` `floor_mod` are, on an unsigned type, `shr` `div` `mod` `lt` `le` `gt` `ge`
`div` `mod`, and do not take a signed type. `count_ones` `leading_zeros` `trailing_zeros` count the bits of the width and
give an `int` (the width for 0); `byte_swap` reverses the bytes of the width. `rotl` `rotr` `mul_hi` `u_mul_hi` take
`u64` only.]

#para("5")[A narrow integer is, as `int` is, the type of globals, of cells of arrays and lists, of values of queues, of
parameters and results and of host word signatures (19.2), and the base of a declared type (7.4), as in `type opcode
u8`. A cell of an array, list or queue and a field of a record take the bytes of the width (`array a u8 1024` is
1024 bytes); one global is an 8-byte place. A character literal is a `u8` or a `u32` (6.5). `case` compares a narrow
integer (10.3). A host word takes and gives the value as one 64-bit integer.]

#ex("spec", "07-narrow.pnx")

#part("Constraints")

#para("6")[An `int` with a narrow integer, or two narrow integers of different types, given to one word is refused. A
constant outside the range converted is refused.]
#ex("spec", "07-narrow-mix.pnx")
#ex("spec", "07-narrow-range.pnx")

#part("Faults")

#para("7")[A result or a conversion outside the range ends the run with the fault `overflow`.]
#ex("spec", "07-narrow-overflow.pnx")

#part("Diagnostics")
#diag(
  ([A value that is not a `bool` where a condition goes], "a bool is expected here"),
  ([A narrow integer with an `int`, or two narrow integers of different types], "this word takes two values of one type: u8 with u8, int with int; u8 and raw convert"),
  ([A constant outside the range converted], "the constant is outside the range of this type; low_u8 and the others keep the low bits"),
  ([A value of a declared type converted directly], "u8, u16, u32, u64, i8, i16, i32 and low_u8 ... are made from an int or from one another; for a declared type take raw first"),
  ([`u_shr` on a signed narrow integer], "u_shr takes an int or an unsigned type; a signed type shifts with shr"),
  ([`u_div` `u_mod` `floor_div` `floor_mod` on a signed narrow integer], "u_div, u_mod, floor_div and floor_mod take an int or an unsigned type"),
  ([`u_lt` `u_le` `u_gt` `u_ge` on a signed narrow integer], "u_lt, u_le, u_gt and u_ge take an int or an unsigned type; a signed type compares with lt"),
  ([`rotl` `rotr` on a narrow integer other than `u64`], "rotl and rotr take an int or a u64; for a narrow type take raw"),
  ([`mul_hi` `u_mul_hi` on a narrow integer other than `u64`], "mul_hi and u_mul_hi take an int or a u64; for a narrow type take raw"),
  ([`abs` on a narrow integer], "abs takes an int or a declared type over int; a narrow type has no abs"),
  ([The two values of an arithmetic word differ in type or are not `int`], "this word takes two int values, or two of the same declared type"),
  ([Arithmetic outside (3) and (3a) on a declared type], "a declared type is added to and subtracted from its own type, and multiplied, divided or taken modulo by an int; for other arithmetic take raw"),
  ([An argument of another type than the signature], "an argument has another type than the word's signature asks for"),
  ([`str` for a global and the like], "stored text is str(N); a str view lives only in locals, arguments and results"),
  ([`str` for a field of a record], "a field holds str(N); a str view lives only in locals, arguments and results"),
  ([Two `dec` of different scales], "two dec values of different scales: rescale one first (d 2 rescale)"),
  ([A `dec` divided by a `dec`], "a dec is divided by an int, not by a dec; for a share, rescale first and divide by the int"),
  ([`dec(A) dec(B) mul` beyond 18 digits], "dec(A) times dec(B) is dec(A+B), and A+B is at most 18; rescale one first"),
  ([A scale outside 0 to 18], "dec(N) has 0 to 18 digits after the point"),
)

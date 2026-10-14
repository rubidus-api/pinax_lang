#import "/tools/common.typ": *

= Core words <core>

#para("1")[A #term("core word") is written as a bare name, without `use`. The tables of this clause write signatures as
in 9.2. `T` means "any one type"; it is a notation for built-in words only. The stack words (`dup` `drop` `swap` `over`
`nip`) are in 9.3 and the combining words that take stems in 10.6.]

== Integers <core-int>

#para("1")[Arithmetic. Overflow and division by zero are faults (11.3).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`add` `sub` `mul`], [`(int, int gives int)`], [add, subtract, multiply],
  [`div` `mod`], [`(int, int gives int)`], [quotient rounded toward zero and its remainder (sign of the dividend)],
  [`floor_div` `floor_mod`], [`(int, int gives int)`], [quotient rounded down and its remainder (sign of the divisor)],
  [`neg` `abs`], [`(int gives int)`], [negation, absolute value; overflow for the smallest `int`],
  [`min` `max`], [`(int, int gives int)`], [the smaller, the larger],
  [`clamp`], [`(n int, lo int, hi int gives int)`], [`n` brought between `lo` and `hi`],
)]

#para("2")[Words that treat overflow otherwise.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`wrap_add` `wrap_sub` `wrap_mul`], [`(int, int gives int)`], [wrap modulo 2#super[64]],
  [`sat_add` `sat_sub` `sat_mul`], [`(int, int gives int)`], [stop at the edge of the range],
  [`chk_add` `chk_sub` `chk_mul` `chk_div`], [`(int, int gives int or fail)`], [fail with `err.overflow` on overflow, `err.div_zero` on division by zero],
)]

#para("3")[Words that read numbers as unsigned: the 64 bits as a number from 0 to 2#super[64]−1.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`u_div` `u_mod`], [`(int, int gives int)`], [unsigned quotient and remainder; division by zero is a fault],
  [`u_lt` `u_le` `u_gt` `u_ge`], [`(int, int gives bool)`], [unsigned comparison],
  [`mul_hi` `u_mul_hi`], [`(int, int gives int)`], [the upper 64 bits of the 128-bit product (signed, unsigned)],
)]

#para("4")[Bits. Shift and rotate counts are taken modulo 64 (their low 6 bits). `shl` does not check for overflow.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`bit_and` `bit_or` `bit_xor`], [`(int, int gives int)`], [bit by bit],
  [`bit_not`], [`(int gives int)`], [every bit flipped],
  [`shl` `shr` `u_shr`], [`(n int, k int gives int)`], [left, signed right, right filling with zeros],
  [`rotl` `rotr`], [`(n int, k int gives int)`], [rotate],
  [`count_ones` `leading_zeros` `trailing_zeros`], [`(int gives int)`], [the number of 1 bits, of leading and trailing zeros (64 for 0)],
  [`byte_swap`], [`(int gives int)`], [the eight bytes in reverse order],
)]

#ex("spec", "15-integers.pnx")

#part("Constraints")

#para("5")[Integer words take `int` only (or two values of the same declared type, 7.4).]
#ex("spec", "15-max-str.pnx")

#part("Faults")

#para("6")[`neg` and `abs` overflow at −2#super[63].]
#ex("spec", "15-neg-min.pnx")

== Comparison and logic <core-logic>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`eq` `ne`], [`(T, T gives bool)`], [equal, not equal: `int` `bool` `str`, declared types, rows, error codes],
  [`lt` `le` `gt` `ge`], [`(T, T gives bool)`], [`int` (as signed), `str` (in byte order), declared types],
  [`and` `or`], [`(bool, bool gives bool)`], [logical and, or; both values are computed],
  [`not`], [`(bool gives bool)`], [negation],
  [`select`], [`(bool, T, T gives T)`], [the first value when true, the second when false; no branch],
)]

#ex("spec", "15-compare.pnx")

#part("Constraints")

#para("2")[The two values compared shall have the same type; so shall the two values of `select`.]
#ex("spec", "15-compare-mixed.pnx")
#ex("spec", "15-select-types.pnx")

== Collections <core-collections>

#para("1")[The words for the storage clause 8 declares. `c` is the name of that storage.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*For*], [*Meaning*],
  [`c len`], [array, list, queue, buffer], [what it holds now (an array: its capacity; a buffer: bytes); pools do not count their rows],
  [`c capacity`], [array, list, queue, buffer], [the declared capacity],
  [`c is_empty`], [array, list, queue, buffer], [is it empty],
  [`c is_full`], [array, list, queue], [is it full],
  [`c clear`], [array, list, queue, buffer], [empties it (an array: every cell to 0); a pool is emptied by freeing its rows],
  [`i c idx`], [array, list], [cell `i`; out of range is the fault `range`],
  [`v c push` / `c pop`], [list], [put at the end / take from the end (`or fail`)],
  [`c add_row`], [list of records], [adds an empty row and gives it (`or fail`)],
  [`v c put` / `c take` / `c peek`], [queue], [put at the back / take from the front / look at the front (`or fail`)],
  [`c alloc` / `r c free` / `r c is_alive`], [pool], [take a row (`owned`, `or fail`) / give it back / is it alive],
  [`s c append` `n c append_int` `b c append_byte`], [buffer], [append (`or fail`)],
  [`c contents`], [buffer], [a view of what it holds],
  [`n c truncate`], [buffer], [cut the length to `n`],
  [`c none` / `r is_none` / `r row_index`], [table of records], [the none row / is it none / the row's number],
  [`k x lookup` / `r x insert` / `r x remove`], [index], [find (`or fail`) / put in (`or fail`) / take out],
)]

#para("2")[Examples are in clause 8.]

== Other words <core-other>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`mark`], [`(gives owned mark)`], [the current place of the region (12.2)],
  [`release`], [`(owned mark)`], [rewinds the region to that place (12.2)],
  [`region_free`], [`(gives int)`], [the bytes left in the region],
  [`call`], [as the block's signature], [calls a block value (10.5)],
  [`raw`], [`(D gives base)`], [a value of a declared type as its base type (7.4)],
  [`err_text`], [`(err gives str)`], [the name of an error code (11.1)],
  [`assert`], [`(bool)`], [the fault `failed` when false],
  [`spawn` `run_all` `resume` `is_done` `cancel` `yield`], [clause 14], [tasks],
)]

#part("Diagnostics")
#diag(
  ([A value that is not `int` given to an integer word], "this word takes two int values"),
  ([Two values of different types compared], "the two values compared must have the same type"),
  ([The values of `select` differ in type], "select takes a bool and two values of one type: flag a b select"),
)

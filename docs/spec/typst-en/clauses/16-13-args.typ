#import "/tools/common.typ": *

== `args` --- values of mixed kinds <std-args>

#part("Syntax")
```ebnf
args-value = "args" { body } "." ;              (* each body leaves one value, 9.8 *)
param      = NAME "args" ;                      (* a parameter that takes an args value *)
```

#para("1")[The module `args` and the type `args` are part of the core: unlike the other modules of this clause, the procedures of
`args` are written `args.WORD` without a `use`. An #term("args value") holds a number of values of different kinds, and a
procedure that takes a varying number of values takes one of them, in a parameter `NAME args`
(`def show label str, rest args gives str do … end`).]

#para("2")[`args v1 v2 … .` makes an args value of the values that the code between `args` and the period leaves; each item
leaves one value. The values are `int`, `bool`, `str`, `dec N` or `float`; a number of another type, a record or a collection is not
one of them. An args holds from 1 to 64 values. The values are copied into the region (12.2): a `str` is copied with
its bytes, so the args value does not depend on where the text was. Written right before `cat`, the values are not built:
`args a b . cat` joins them as 9.8 says.]

#para("3")[An args value has a life down the calls only: it is a parameter. It is not stored in a local or a global, not
given back as a result and not a value of another args. It goes on to any procedure that takes an `args` (`rest inner`),
and `rest cat` writes its values as `args … . cat` writes them: a `str` as it is, an `int` in decimal, a `bool` as
`true` or `false`, a `dec N` with all its `N` digits, a `float` as `str.to_str` writes it (7.7).]

#ex("spec", "16-args.pnx")

#para("4")[The procedures below read an args value, and take the number of a value first (0 is the first) and the args last.
The kinds are the constants `args.k_int` 1, `args.k_bool` 2, `args.k_str` 3, `args.k_dec` 4 and `args.k_float` 5.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Procedure*], [*Signature*], [*Meaning*],
  [`len`], [`(a args gives int)`], [the number of values],
  [`kind`], [`(i int, a args gives int or fail)`], [the kind of value `i`],
  [`int`], [`(i int, a args gives int or fail)`], [value `i` when it is an `int`],
  [`bool`], [`(i int, a args gives bool or fail)`], [value `i` when it is a `bool`],
  [`str`], [`(i int, a args gives str or fail)`], [value `i` when it is a `str`: a view of the bytes copied into the args value],
  [`dec`], [`(i int, N int, a args gives dec N or fail)`], [value `i` when it is a `dec N`; `N` is a constant 0 to 18],
  [`float`], [`(i int, a args gives float or fail)`], [value `i` when it is a `float`],
)]

#ex("spec", "16-args-read.pnx")
#ex("spec", "16-args-float.pnx")

#para("5")[A procedure that reads a value of another kind than the one it is, or a `dec` of other digits than `N`, fails with
`err.format`. A number out of range, negative or not less than `len`, fails with `err.not_found`.]

#ex("spec", "16-args-pass.pnx")

#part("Constraints")

#para("6")[An `args` is only a parameter. Storing it in a local or a global, giving it back in `gives`, holding it in
another args, or giving it to a host procedure or an `extern` is refused; so is an `args` with no value.]
#ex("spec", "16-args-stored.pnx")
#ex("spec", "16-args-nested.pnx")

#part("Faults")

#para("7")[An args value is made in the region. When the region is too small for it, the fault is `full` (12.2 (5)).]
#ex("spec", "16-args-full.pnx")

#part("Diagnostics")
#diag(
  ([An args stored, or given back], "an args is only a parameter: it is not stored in a local or a global, and not given back"),
  ([An args inside an args], "an args holds no args: its values are int, bool, str, dec N and float"),
  ([A value that is not int, bool, str, dec N or float], "an args holds int, bool, str, dec N and float values"),
  ([A procedure of args with its values in the wrong order], "this procedure takes the number of the value, and then the args: 0 rest args.int"),
  ([The digits of `args.dec` not a constant], "the digits of args.dec are a constant 0 to 18: 2 rest args.dec"),
  ([`cat` with no args before it], "cat takes args: args a b . cat, or an args parameter"),
)

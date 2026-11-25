#import "/tools/common.typ": *

== `f` --- floating-point computation <std-f>

#para("1")[The module `f` holds the procedures of `float` (7.7); `use f .` opens it. The four arithmetic procedures round
the exact value once to the nearest `float` (ties to even). No procedure makes a fault (`f.to_int` excepted): a NaN and
an infinity pass on as results (7.7 (2)).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`add` `sub` `mul` `div`], [`(a float, b float gives float)`], [`a + b`, `a - b`, `a × b`, `a ÷ b`; a division by zero gives an infinity or a NaN],
  [`neg`], [`(a float gives float)`], [the sign changed; a NaN stays a NaN],
  [`abs`], [`(a float gives float)`], [the sign removed],
  [`near`], [`(a float, b float, eps float gives bool)`], [true when `a eq b`, or when `a b f.sub f.abs` is at most `eps`; false when `a` or `b` is a NaN],
  [`is_nan`], [`(a float gives bool)`], [true when `a` is a NaN],
  [`is_finite`], [`(a float gives bool)`], [true when `a` is neither an infinity nor a NaN],
  [`from_int`], [`(n int gives float)`], [the `float` nearest to `n` (ties to even)],
  [`to_int`], [`(a float gives int)`], [the `int` cut toward zero; the fault `overflow` for a NaN or a value out of range],
)]

#ex("spec", "16-f.pnx")

#part("Diagnostics")
#diag(
  ([`f.neg` `f.abs` of a value that is not a `float`], "f.neg and f.abs take a float"),
  ([`f.near` of a value that is not a `float`], "f.near takes three float values: a b eps f.near"),
  ([`f.is_nan` `f.is_finite` of a value that is not a `float`], "f.is_nan and f.is_finite take a float"),
  ([`f.from_int` of a value that is not an `int`], "f.from_int takes an int (str.to_float reads a str)"),
  ([`f.to_int` of a value that is not a `float`], "f.to_int takes a float (str.to_int reads a str)"),
)

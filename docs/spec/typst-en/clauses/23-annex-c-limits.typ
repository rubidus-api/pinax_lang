#import "/tools/common.typ": *
// shape: numbers

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex C" } else { "C." + n.pos().slice(1).map(str).join(".") })
= Size limits (normative) <annex-limits>

#para("1")[A conforming processor shall accept the sizes of the "minimum" column. The "this implementation" column has
this implementation's values. Default capacities differ between processors (4 (5)); a host changes them through
`pinax_config`.
#table(columns: (1fr, auto, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Item*], [*Minimum*], [*This implementation*],
  [length of a name (bytes)], [31], [31],
  [length of a `ptn` pattern (bytes)], [255], [255],
  [captures of a `ptn` pattern], [9], [9],
  [steps of one `ptn` word], [2#super[24]], [2#super[24]],
  [C arguments of an `extern` function], [8], [8],
  [`str` arguments, results of a host word], [8, 4], [8, 4],
  [host modules' words (per state)], [64], [64],
  [handle types], [16], [16],
  [digits after the point of `dec(N)` (7.5)], [18], [18],
  [globals of a state (every load together)], [1024], [3839],
  [`defer`s in one word (10.7)], [16], [16],
  [locals kept after a fault], [---], [16, 1 KB],
  [default integer slots / string slots], [---], [4096 / 1024],
  [default region of the main run], [---], [65536 bytes],
  [default handles alive at once], [---], [64],
  [default call depth / region of a task], [---], [16 / 1024 bytes],
)]

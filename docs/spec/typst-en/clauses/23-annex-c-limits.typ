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

#para("2")[These are the limits this implementation puts on a source (informative). Going beyond one is refused at
load, and the message names the limit. Another processor may have other values.
#table(columns: (1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Item*], [*This implementation*],
  [Stack depth inside a definition (values)], [64],
  [Blocks, branches and loops open inside each other], [32],
  [Block values inside block values], [4],
  [Locals of one word], [96],
  [Inputs / results of a signature], [16 / 16],
  [Modules one source uses], [16],
  [Values of one `case` branch], [32],
  [N of `str(N)` (bytes)], [255],
  [Size of one record (bytes)], [4095],
  [Length of a signature text (bytes)], [71],
  [`mark`s of one word], [255],
  [Sources of a state], [256],
  [Tasks of a set / region of one task], [4096 / 16 MiB],
  [Instructions of a `rex` program / nesting of groups], [4096 / 64],
)]

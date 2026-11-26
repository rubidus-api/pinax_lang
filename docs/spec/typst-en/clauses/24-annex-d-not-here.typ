#import "/tools/common.typ": *
// shape: what this edition leaves out

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex D" } else { "D." + n.pos().slice(1).map(str).join(".") })
= Not in this edition (informative) <annex-later>

#para("1")[The following have a place in the design but are not accepted by the processor of this edition; using them
is refused at load. This document does not define their meaning until a later edition does.
- The rest of `float` (7.7): the module `math` of mathematical functions (square root, floor, exponential, logarithm,
  trigonometric functions), record fields `f32` `f64`, types declared over `float`, `match`, and real arguments of `extern`.
- Declarations defined by users (`make`, 6.6 (5)).
- Records held inside records, and the field-by-field layout `columns`.
- The compression module `deflate`.
- Record fields of a declared type, of `err` or of a handle type; a field takes the widths of 8.8 only.]

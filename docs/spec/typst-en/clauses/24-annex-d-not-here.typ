#import "/tools/common.typ": *
// shape: what this edition leaves out

#set heading(numbering: (..n) => if n.pos().len() == 1 { "Annex D" } else { "D." + n.pos().slice(1).map(str).join(".") })
= Not in this edition (informative) <annex-later>

#para("1")[The following have a place in the design but are not accepted by the processor of this edition; using them
is refused at load. This document does not define their meaning until a later edition does.
- Floating point: the type `float`, float literals (6.4 (8)), float slots, the module `float`, `fmt.fixed`, record
  fields `f32` and `f64`.
- Declarations defined by users (`make`, 6.6 (5)).
- Records held inside records, and the field-by-field layout `columns`.
- The compression module `deflate`.]

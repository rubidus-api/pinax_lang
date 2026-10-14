#import "/tools/common.typ": *

= Memory <memory>

== Overview <memory-overview>

#para("1")[All the storage of a state is in the block the host gave (5.1) and is placed at load. No storage appears
while running. There is no garbage collector, and no value refers to another by address.]

#para("2")[Values lie in three kinds of places:
- declared storage (clause 8): globals, arrays, lists, queues, pools, buffers; their declarations fix their size;
- slots of the stack and of locals; their depth is fixed at load (9.3);
- one *region* for each task (12.2), where the bytes of text made while running go.]

#para("3")[So the upper bound of the memory a program uses is known at load. Using up the region or a capacity shows as
a fault or a failure while running; it never leads the processor to ask for more memory.]

== The region and marks <memory-region>

#para("1")[Words that make new bytes --- `cat`, `str.upper`, the words that turn numbers into text and so on --- write
the result into the region and give a view of it. The region is filled from the front. The word `region_free` gives the
number of bytes left.]

#para("2")[`mark` gives the current place in the region as an `owned` value (type `mark`). `mark release` rewinds the
region to that place; every byte made after the mark can be used again. A loop that makes much text marks and releases
in every turn.]

#para("3")[Bytes put into stored text (`str(N)`) are copied there, so they survive rewinding the region.]

#ex("spec", "12-region.pnx")

#para("4")[A `str` result the host receives can be used only until the next call on that state. Text the host keeps
longer it copies into its own memory (clause 19).]

#part("Faults")

#para("5")[A region too small is the fault `full`.]
#ex("spec", "12-region-full.pnx")

== The life of views <memory-views>

#para("1")[A view into the region cannot be used once its bytes are rewound. A processor checks this at load. Within
one body (a word, a block value, the code outside definitions) it counts, for each `str` value, the marks made before
it. `mark release` takes back the values made after that mark.]

#part("Constraints")

#para("2")[A `release` while a value it takes back is still on the stack is refused.]
#ex("spec", "12-release-stack.pnx")

#para("3")[A local holding a value taken back cannot be read until something is stored into it again.]
#ex("spec", "12-release-view.pnx")

#para("4")[The check leans to caution. String literals are not in the region and are never taken back. Every other
`str` --- views, stored text read back, values words give --- counts as made where it appears. Where paths meet, a value
counts as the latest and a mark as the earliest. So it may refuse a correct program; then keep the value in a `str(N)`
or move the mark.]

#para("5")[Handing an `owned` mark to a word counts as a `release` there. A word that takes a mark counts its `str`
inputs as made after that mark. Two tasks sharing one region are not followed by this check (clause 14).]

== Owned values <memory-owned>

#para("1")[`owned` before a type means an #term("owned value"): an open file and other handles, a row taken from a pool,
a mark of the region. A processor checks at load that such a value is handed over exactly once on every path.]

#para("2")[An `owned` value is used in two ways:
- #term("borrowing"): giving it to a word that takes the type without `owned` (`f file.write`); any number of times;
  the value stays;
- #term("handing over"): giving it to a word that takes it `owned` (`f file.close`), storing it in a global or field,
  or returning it as an `owned` result; after that the name is empty and cannot be read.]

#para("3")[An `owned` value is put into a local or handed over at once. It cannot be doubled with `dup` or discarded
with `drop`. It shall be handed over on the paths of failures (`or_fail` `or_return`) and at `return` too.]

#ex("spec", "12-owned-file.pnx")

#part("Constraints")

#para("4")[Using a value again after handing it over is refused.]
#ex("spec", "12-owned-twice.pnx")

#para("5")[Doubling or discarding an `owned` value is refused.]
#ex("spec", "12-owned-dup.pnx")

#para("6")[A single path that does not hand it over is enough to refuse.]
#ex("spec", "12-owned-path.pnx")

#para("7")[In a run that ends with a fault, the processor closes the handles that run opened (11.3 (3)).]

#part("Diagnostics")
#diag(
  ([A value taken back is on the stack at `release`], "a str made after this mark is still on the stack; the release gives its bytes back"),
  ([Reading a value taken back], "this str was made after a mark that was released since; the region may hold other bytes there now"),
  ([Reading a value handed over], "this local gave its owned value away; it holds nothing to read"),
  ([Doubling or discarding an `owned` value], "an owned value is kept in a local or given on at once; this word would lose it"),
  ([A path that does not hand it over], "an owned value in a local is not given on every path (close, free or store it)"),
)

#import "/tools/common.typ": *
// shape: definitions only

= Terms <terms>

#para("1")[In this document the following words are used in these senses only. A term defined in a later clause is
set in bold where it is defined.]

#para("2")[#term("processor"): an implementation that reads, checks and runs source; the library and the command `pnx`
that uses it.]

#para("3")[#term("host"): the C program that embeds a processor. It provides memory, registers words and calls words of
scripts.]

#para("4")[#term("state"): one unit of execution of a processor. Everything is inside one block of memory the host
gave it. Two states share nothing.]

#para("5")[#term("source"): Pinax text loaded at once, one file or one string. One source is one module (clause 13).]

#para("6")[#term("load"): reading a source, checking it and adding it to a state. When a load refuses, the state is as
it was before.]

#para("7")[#term("refusal"): a load not accepting a source. The processor reports the place (line and column) and the
reason. No code of a refused source runs.]

#para("8")[#term("word"): an action called by name. Core words, words of standard modules, words a script defines and
words a host registers all look the same where they are called.]

#para("9")[#term("signature"): what a word takes from the stack, what it gives, and whether it can fail (9.2).]

#para("10")[#term("stack"): where values pass from word to word. Its depth and the type of each place are fixed at
load.]

#para("11")[#term("failure"): a word whose signature has `or fail` reporting that it could not do its work. The caller
must handle it at the next token (11.2).]

#para("12")[#term("fault"): something that ends the run on the spot (overflow, division by zero, an index out of range
and so on). A script cannot catch it; the host receives it (11.3).]

#para("13")[#term("region"): where the bytes of strings made while running are placed. Each task has one; a mark
rewinds it (12.2).]

#para("14")[#term("view"): a string value that points at bytes without copying them; type `str` (7.3).]

#para("15")[#term("task"): a flow of execution that takes turns with others inside one state; it changes only where it
yields (clause 14).]

#para("16")[#term("module"): a named collection of words and values: standard modules, host modules, modules written in
Pinax (clause 13).]

#para("16a")[#strong[Run] (of a word): from when the host starts a word (`pinax_run`, the code of a load, a line of
`pinax_eval`) until that word ends or stops with a fault. Stopping on the budget and going on again is the same run
(19.5).]

#para("16b")[#term("budget"): the number of instructions one run may execute at a time. When it is used up the run
stops and returns to the host, which goes on with it. 0 means no limit (19.2).]

#para("17")[In this document "shall" and "cannot" bind programs. A program that breaks them does not conform; the
processor refuses it (what can be known at load) or stops with a fault (what can be known only while running). Each
rule says which. "May" is a permission.]

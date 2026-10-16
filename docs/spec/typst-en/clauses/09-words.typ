#import "/tools/common.typ": *

= Words <words>

== Definitions <words-definition>

#part("Syntax")
```ebnf
definition = [ "export" ] "word" NAME "(" effect ")" { body } "end" ;
effect     = [ params ] [ "gives" params ] [ "or" "fail" ] ;
params     = param { "," param } ;
param      = [ NAME ] type ;
type       = [ "owned" ] ( NAME [ "(" size ")" ] | "word" "(" effect ")" ) ;
```

#para("1")[`word NAME (signature) body end` defines a *word*. Words are defined only outside definitions, at the
outermost level of a source; a word cannot be defined inside another.]

#para("2")[To #term("call") a word is to write its name in a body. Where it is called, the topmost values of the stack
become its inputs, and when it ends its results are in their place. Core words, words of standard modules and host
words are called the same way.]

== Signatures <words-signature>

#para("1")[Every word has a *signature*: its inputs, then `gives` and its results, then `or fail` if it can fail. A word
with neither inputs nor results has `()`. Signatures are checked at load: the stack where a word is called shall match
its inputs, and the stack where its body ends shall match its results.]

#para("2")[An input with a name (`w int`) becomes a local of that name, and the body starts with an empty stack. An
input given as a type only (`int`) stays on the stack when the body starts. Names on the results side are for the
reader only and make no locals. An input may also take a collection (9.9).]

#ex("spec", "09-signatures.pnx")

#part("Constraints")

#para("3")[The inputs of a signature shall be either all named or all bare types.]
#ex("spec", "09-signature-mixed.pnx")

#para("4")[Where the body ends (and at every `return`) the stack shall hold exactly the results, in order and type.
More or fewer values are refused.]
#ex("spec", "09-result-missing.pnx")

== The stack <words-stack>

#para("1")[Values passed between words lie on the *stack*. A literal or a name pushes one value; a word takes its inputs
off and pushes its results. At load a processor knows, at every place of a body, the depth of the stack and the type of
each slot.]

#para("2")[Only five core words rearrange the stack.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Before*  →  *after*], [*Meaning*],
  [`dup`], [`a` → `a a`], [one more of the top],
  [`drop`], [`a` → ], [discard the top],
  [`swap`], [`a b` → `b a`], [exchange the top two],
  [`over`], [`a b` → `a b a`], [one more of the second, on top],
  [`nip`], [`a b` → `b`], [discard the second],
)
There are no deeper words (`rot` `pick` `roll` `depth`). When three or more values are involved, use locals.]

#ex("spec", "09-stack-words.pnx")

#note[For a reader new to a stack, here is `2 3 add 4 mul` one token at a time (the top on the right).
#trace(
  ("2", "2", [A value goes on.]),
  ("3", "2 3", [A value goes on.]),
  ("add", "5", [Two values come off; their sum goes on.]),
  ("4", "5 4", [A value goes on.]),
  ("mul", "20", [Two values come off; their product goes on.]),
)
At load the processor knows the second column of this table (depth and types) at every place in advance. That is why
the mistakes of (3) and (3a) show before anything runs.]

#part("Constraints")

#para("3")[A word given fewer values than it takes is refused. The depth of the stack does not change while running.]
#ex("spec", "09-stack-short.pnx")

#para("3a")[When the code outside definitions (5.2) ends, at the end of the source, values left on the stack are
refused. The stack at the end of a word's body is 9.2's.]
#ex("spec", "09-leftover.pnx")

#para("4")[Using a name that is not defined is refused.]
#ex("spec", "09-unknown-word.pnx")

== Locals <words-locals>

#part("Syntax")
```ebnf
store = "to" ( NAME | PATH | "(" NAME { "," NAME } ")" ) ;
set   = "set" ( NAME | PATH ) ;
```

#para("1")[In a body, `to NAME` takes the top value off and puts it in that name. When the name is new, a
#term("local variable") of the value's type is made; later `to`s shall store the same type. `to` stores only into locals
(and into fields, with `to r.FIELD`).]

#para("1a")[A global is stored with `set NAME`: `v set NAME` for a scalar global, `v i set NAME` for a cell of an array or
a list. A global another module exports is `set MODULE.NAME`. Inside and outside definitions alike. The rule makes the
text show whether a store makes a local or changes a global.]

#para("2")[`to (a, b, c)` stores the top three values at once. The rightmost name takes the top value --- the same
order as in a signature.]

#ex("spec", "09-locals.pnx")
#ex("spec", "09-store-global.pnx")

#para("3")[A local is visible from where it is first stored to the end of the branch, loop or block around it. A local
made outside can be stored into inside.]

#part("Constraints")

#para("4")[Storing a value of another type into a local is refused.]
#ex("spec", "09-local-type.pnx")

#para("5")[A local first made inside a branch cannot be read outside it: coming by the other branch, it would hold no
value.]
#ex("spec", "09-local-scope.pnx")

#para("6")[`to` with the name of a global is refused. A global cannot be hidden (9.5), so no local of that name can be
made, and the global is not changed silently either; the message names `set`. `set` with a name that is not a global is
refused.]
#ex("spec", "09-store-global-bare.pnx")

== No hiding <words-shadowing>

#para("1")[A new name (a parameter, a local, the name of a loop) cannot be the same as a name visible there --- a word,
a global, a constant, a type, a local outside. One name means one thing at one place.]

#part("Constraints")

#para("2")[A parameter or local with the name of a visible name is refused.]
#ex("spec", "09-shadow.pnx")
#ex("spec", "09-shadow-word.pnx")

== Recursion and tail calls <words-recursion>

#para("1")[A word may call itself. No forward declaration is needed.]

#para("2")[A #term("tail call") --- a call that is the last thing a word does --- reuses the caller's place. Recursion
through tail calls does not grow the depth. It needs no mark.]

#ex("spec", "09-recursion.pnx")

#part("Faults")

#para("3")[Calls that are not tail calls pile up; past the depth limit (annex C) they stop with the fault `depth`.]
#ex("spec", "09-depth.pnx")

== Forward words <words-forward>

#para("1")[`forward word NAME (signature)` announces a word defined further on, for two words that call each other.]

#ex("spec", "09-forward-word.pnx")

#part("Constraints")

#para("2")[The definition shall have the announced signature, in the same module; otherwise it is refused.]
#ex("spec", "09-forward-other.pnx")

== Joining text <words-cat>

#part("Syntax")
```ebnf
cat    = "cat" "(" phrase { "," phrase } ")" ;
phrase = body { body } ;                   (* code that leaves one value *)
```

#para("1")[`cat ( … )` joins the comma-separated items into one `str`. Each item is code that leaves one value. A `str`
goes in as it is, an `int` in decimal, a `bool` as `true` or `false`. Other forms (hexadecimal and so on) are made into
a `str` first by the words that make them (16.2).]

#para("2")[The result is a view of bytes made in the region (12.2). The number of items is what the text shows; the
processor adds up the length first and writes once.]

#ex("spec", "09-cat.pnx")

== Collections as parameters <words-collections>

#part("Syntax")
```ebnf
collection-type = ( "array" | "list" | "queue" ) cell | "buffer" ;
cell            = NAME [ "(" size ")" ] ;   (* int, bool, err, str(N), or a record *)
```

#para("1")[An input of type `array T`, `list T` or `queue T`, or of type `buffer`, takes a collection: the caller writes
the collection's name there (`scores total`). In the body the parameter is used wherever the name of such a collection
goes --- `l len`, `i l idx`, `v l push`, `v i set l`, `l do … end each`, `… end l map_into`, `q take`, `s b append` and
so on. The cells shall
be of the type the parameter says: `int` `bool` `err`, `str(N)` with the same `N`, or a record. The capacity is not part
of the type; any capacity is taken, and `l capacity` gives the one it has.]

#ex("spec", "09-coll-param.pnx")

#para("2")[In a word with a parameter `l` that is a table of records, the name `l` is also the type of its rows, as the
name of a declared table is (8.8). A row reached through `l` is used in that word: in its locals, its fields and the
blocks of its loops.]

#ex("spec", "09-coll-param-rows.pnx")

#para("2a")[`map_into` and `filter_into` (10.6) fill a list or an array a parameter names, and copy rows from a table a
parameter names.]

#ex("spec", "09-coll-param-fill.pnx")

#para("3")[What is passed is the collection itself, not a copy. Collections are globals, so a parameter may be held
across `yield` and the waits of 14.3.]

#part("Constraints")

#para("4")[A collection shall be given where its kind and cells are those of the parameter.]
#ex("spec", "09-coll-param-kind.pnx")

#para("5")[A collection parameter is not a value: it is not stored with `to`, not a result of a word, and not a
parameter of a block (`word (…)`). Pools and indexes are not taken.]
#ex("spec", "09-coll-param-result.pnx")

#para("6")[A word that truncates a list parameter (8.5 (2a)), or gives it to a word that does, truncates the list given
to it. The rule of 8.5 (2b) then holds for that list, where the word is called.]
#ex("spec", "09-coll-param-truncate.pnx")

#para("7")[`filter_into` does not copy rows when both tables are parameters. Walk one with `each` and fill the other
with `add_row`.]
#ex("spec", "09-coll-param-copy.pnx")

#part("Diagnostics")
#diag(
  ([Named and bare inputs mixed], "inputs are all named or all bare types, not a mix"),
  ([The stack at the end unlike the results], "the stack does not hold exactly the results of the signature here"),
  ([Too few values on the stack], "the stack does not hold enough values for this word"),
  ([Values left at the end of the source], "values are left on the stack at the end of the source"),
  ([A name that is not defined], "this name is not declared before this place"),
  ([A local given another type], "this local variable has another type than the value stored into it"),
  ([`to` with the name of a global], "this name is a global; a global is written with set: value set name"),
  ([`to MODULE.NAME` into a global], "a global of a module is written with set: value set module.name"),
  ([`set` with the name of a local], "set writes a global; a local is written with to"),
  ([`set` with a name that is not a global], "set writes a declared global; there is none of this name"),
  ([Hiding a visible name], "this name is already visible here; a new name may not hide it"),
  ([A signature unlike the forward declaration], "the definition has another signature than its forward declaration"),
  ([A collection of another kind or cells], "the collection given here is of another kind, or holds another type, than the parameter says"),
  ([A value where a collection goes], "this parameter takes an array, list, queue or buffer: give its name"),
  ([A collection as a result], "a word gives no collection; it takes one as a parameter"),
  ([A kept list given to a word that truncates it], "this list's rows are kept by a field, a global, a named row or an index; the word it is given to truncates it"),
  ([Rows copied between two tables parameters name], "filter_into does not copy rows between two tables parameters name; walk one and add_row"),
)

#import "/tools/common.typ": *

= Words and signatures <words>

#opening[Chapter 1 (making words), chapter 2 (the stack).][
- What goes into a signature, and what does the processor check with it?
- What is the difference between naming an input and not?
- How are words with two results, and words that call themselves, written?
]

== What a signature does

Every word has a #strong[signature]: its inputs, `gives`, its results, and `or fail` if it can fail. The processor checks
the signature from both sides: where the word is called, that the top values of the stack match the input types; in the
body, that the stack is exactly the results when the body ends.

#ex("manual", "03-temperature.pnx")

`show` takes one `int` and gives one `str`, so `365 c_to_f show` can be one item of a `cat`. Temperatures are kept in
tenths of a degree as `int`s because this edition has no floating point.

== Named inputs and bare inputs

An input with a name (`a int`) becomes a local of that name, and the body starts with an empty stack. An input given as
a type only (`int`) stays on the stack when the body starts. One signature cannot mix the two.

#ex("manual", "03-two-results.pnx")

`divmod` has two results. The names `q` and `r` on the results side are for the reader only; they make no locals. The
caller takes both at once with `to (q, r)`. The rightmost name takes the top value --- in the order of the signature.

#qa[With three or more inputs, which is better?][Name them. Two values can be handled with `dup` and `swap`, but from
three on the stack is hard to follow in one's head. That is also why Pinax has no `rot` or `pick`.]

== Programs that break their signature

#ex("manual", "03-result-wrong.pnx")

When the body ends there are two values on the stack (the result of `w h mul`, and `w`), but the signature promised
one.

#ex("manual", "03-call-wrong.pnx")

`greet` takes a `str`, and was given `42`. The check happens before running, so such a mistake always shows, whether
that line would ever run or not.

#mistake[Naming a variable after a core word][Common words such as `sum`, `count`, `first` and `add` are all names of
core words. Pinax does not let a visible name be hidden by another (no hiding), so parameters and locals with those
names are refused.
#ex("manual", "03-taken-name.pnx")
Use other names such as `total`, `n` or `items`. Reserved words such as `of`, `end` and `text` cannot be names either.]

== Words that call themselves

#ex("manual", "03-recursion.pnx")

A word may call itself without announcing it. A #strong[tail call] --- a call that is the last thing a word does, as
in `gcd` --- reuses the place, so the depth does not grow. A call with work left after it, as in `fib`, piles up depth,
and too deep stops with the fault `depth`. Two words that call each other announce one with `forward word`.

#summary[
- Every word has a signature, checked where it is called and in its body (standard 9.2).
- Named inputs become locals; bare inputs stay on the stack; they do not mix (9.2).
- Several results are taken with `to (a, b)`; the rightmost is the top (9.4).
- A visible name (core words included) cannot be used for a new name (9.5).
- Tail calls do not grow the depth (9.6).
]

#import "/tools/common.typ": *

= Procedures and signatures <words>

#opening[Chapter 1 (making procedures), chapter 2 (the stack).][
- What goes into a signature, and what does the processor check with it?
- What is the difference between naming an input and not?
- How are procedures with two results, and procedures that call themselves, written?
]

== What a signature does

Every procedure has a #strong[signature]: its inputs, `gives`, its results, and `or fail` if it can fail. The processor checks
the signature from both sides: where the procedure is called, that the top values of the stack match the input types; in the
body, that the stack is exactly the results when the body ends.

#ex("manual", "03-temperature.pnx")

`show` takes one `int` and gives one `str`, so `365 c_to_f show` can be one item of a `cat`. Temperatures are kept in
tenths of a degree as `int`s, so the arithmetic is exact; for quantities of unknown scale there is `float` (chapter 2).

A signature is one line that says what the procedure does to the stack. `(tenths int gives int)` means "takes one `int`
off the top and puts one `int` on". So the caller's stack can be followed without reading the body.

#trace(
  ("365", "365", [The value goes on.]),
  ("c_to_f", "977", [As its signature says: one `int` off, one `int` on.]),
  ("show", "\"97.7\"", [One `int` off, one `str` on.]),
)

== What happens at a call

At a call the processor takes as many values off the top of the stack as the signature has inputs, and gives them to
the procedure. #strong[The value put on first is the first input]. The body of the procedure starts with an #strong[empty
stack] --- the caller's values further down can be neither seen nor touched. When the body ends the stack shall hold
exactly the results of the signature, and those values go onto the caller's stack.

#ex("manual", "03-arg-order.pnx")

In `10 3 minus`, `10` went on first, so `a` is 10 and `b` is 3 --- the same as the "first on the left" rule of
subtraction (chapter 2). Follow the body of `half_of_sum`. The caller's stack held `4 8`; inside the body they are the
names `a` and `b`, and the stack starts empty.

#trace(
  ([(the body starts)], "—", [`a` is 4 and `b` is 8. The stack is empty.]),
  ("a b", "4 8", [Using a name puts its value on.]),
  ("add", "12", [Adds.]),
  ("2 div", "6", [Divides by 2. One `int` when the body ends --- as the signature says.]),
)

== Named inputs and bare inputs

An input with a name (`a int`) becomes a local of that name, and the body starts with an empty stack. An input given as
a type only (`int`) stays on the stack when the body starts. One signature cannot mix the two.

#ex("manual", "03-two-results.pnx")

`divmod` has two results. The names `q` and `r` on the results side are for the reader only; they make no locals. The
caller takes both with `to r to q`. The name written first takes the top value --- the reverse of the order of the signature.

`bare_square` has no name for its input. Then the value does not become a name: the body starts #strong[with it still
on the stack]. The body of `7 bare_square` starts with one `7` on the stack, and `dup mul` squares it.

#trace(
  ([(the body starts)], "7", [A bare input stays on the stack.]),
  ("dup", "7 7", [One more.]),
  ("mul", "49", [Multiplies. One result is left.]),
)

One signature cannot mix named and bare inputs: it would be unclear which values stay on the stack.

#ex("manual", "03-mixed-inputs.pnx")

#qa[With three or more inputs, which is better?][Name them. Two values can be handled with `dup` and `swap`, but from
three on the stack is hard to follow in one's head. That is also why Pinax has no `rot` or `pick`.]

== Programs that break their signature

#ex("manual", "03-result-wrong.pnx")

When the body ends there are two values on the stack (the result of `w h mul`, and `w`), but the signature promised
one.

#ex("manual", "03-call-wrong.pnx")

`greet` takes a `str`, and was given `42`. The check happens before running, so such a mistake always shows, whether
that line would ever run or not.

A procedure without results (a signature without `gives`) shall end its body with an empty stack. To give a computed value
back, write the result in the signature: `(n int gives int)`.

#ex("manual", "03-no-result.pnx")

#mistake[Naming a variable after a core procedure][Common procedures such as `sum`, `count`, `first` and `add` are all names of
core procedures. Pinax does not let a visible name be hidden by another (no hiding), so parameters and locals with those
names are refused.
#ex("manual", "03-taken-name.pnx")
Use other names such as `total`, `n` or `items`. Reserved words such as `when`, `end` and `text` cannot be names either.]

== Procedures that call themselves

#ex("manual", "03-recursion.pnx")

A procedure may call itself without announcing it. A #strong[tail call] --- a call that is the last thing a procedure does, as
in `gcd` --- reuses the place, so the depth does not grow. A call with work left after it, as in `fib`, piles up depth,
and too deep stops with the fault `depth`. Two procedures that call each other announce one with `forward NAME signature end`.

#qa[A procedure's body is long and I lose the stack. What do I do?][`pnx see FILE` shows the stack at the end of every line
of a definition (chapter 15). Write the body one step per line, and name the values in the middle with `to NAME`
(chapter 4): then no more than two values are on the stack at once.]

== Values of any number and kind

A procedure takes a fixed number of values of fixed types. When the number or the kinds vary, the procedure takes one value of the
type #strong[args]: a parameter `NAME args`. The caller builds it with `args … .`, any values separated only by spaces and
ended by a period.

#ex("manual", "03-args.pnx")

`rest args.len` is the number of values, `i rest args.kind` the kind of value `i`, and `i rest args.int`, `args.str`,
`args.bool` read one by its kind; a value of another kind is a failure, like an index out of range. An args is made in the
region, and it is a parameter only: it is not stored and not returned, and it goes on to another procedure that takes `args`.

#summary[
- Every procedure has a signature, checked where it is called and in its body (standard 9.2).
- The value put on first is the first input. The body starts with an empty stack (bare inputs stay on it) and ends with
  only its results (9.2).
- Named inputs become locals; bare inputs stay on the stack; they do not mix (9.2).
- Several results are taken with `to b to a`; the name written first takes the top (9.4).
- A visible name (core procedures included) cannot be used for a new name (9.5).
- Tail calls do not grow the depth (9.6).
]

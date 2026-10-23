#import "/tools/common.typ": *

= Values and the stack <values>

#opening[Chapter 1 (the order of code, `pnx`, remarks).][
- What is the stack, and how do values and words move on it?
- How is an ordinary formula put into the order Pinax reads?
- When does the processor check the stack, and what does it refuse?
- What values does Pinax have, and what happens when an integer overflows?
]

This chapter is about the #strong[stack], the ground Pinax stands on. It is the least familiar part for someone who
has used other languages, but it has only a few rules. Follow the examples of this chapter through the tables, one row
at a time, and the rest of the book reads much more easily.

== What the stack is

The #term("stack", en: "stack") is a place where values are piled up, like a pile of plates. A new plate goes on top,
and a plate is taken from the top. Nobody pulls a plate out of the middle.

Pinax code runs on this stack. The processor reads the code left to right, top to bottom, one token at a time, and
does one of two things with each token.
+ A #strong[value] (`2`, `"abc"`, `true`) goes on top of the stack.
+ A #strong[word] (`add`, `io.println`) is set to work. The word takes as many values as it needs off the top, does its
  work, and puts its results, if any, on top.

Follow `2 3 add` by these rules. This book uses tables like the one below often. The top of the stack is written on
the #strong[right], and `—` is an empty stack.

#trace(
  ("2", "2", [A value: it goes on.]),
  ("3", "2 3", [A value: it goes on top of `2`. The top is now `3`.]),
  ("add", "5", [A word: it takes the top two values `2` and `3` off, adds them, and puts the result `5` on.]),
)

What other languages write `2 + 3` is `2 3 add` in Pinax. The values come first and the work after: this is the #strong[postfix]
notation of chapter 1. It looks backwards at first; read it as "lay out what is needed, then set
the work going" and it becomes natural.

== Computing on the stack

#ex("manual", "02-stack.pnx")

Follow the first line, `2 3 add 4 mul`, through a table.

#trace(
  ("2", "2", [Goes on.]),
  ("3", "2 3", [Goes on.]),
  ("add", "5", [Adds the two values.]),
  ("4", "5 4", [Goes on.]),
  ("mul", "20", [Multiplies the two values.]),
  ("str.to_str", "\"20\"", [Turns the integer into text: one value off, one on.]),
  ("io.println", "—", [Prints the text. The stack is empty.]),
)

The second line, `2 3 4 mul add`, piles three values up first. `mul` multiplies the top two, `3` and `4`, and `add`
adds the `2` left below to that result. So it is $2 + 3 times 4 = 14$.

#trace(
  ("2 3 4", "2 3 4", [Three values go on in turn.]),
  ("mul", "2 12", [Multiplies the top `3` and `4`. `2` stays below.]),
  ("add", "14", [Adds `2` and `12`.]),
)

Without parentheses, the order of the computation is the order of the text. Where a word is written decides what is
computed first.

== Turning a formula into postfix

A familiar formula is carried over in this order.
+ With parentheses, carry the inside over first.
+ Write one operation as "left value, right value, operation word": $a - b$ is `a b sub`.
+ Carry multiplication and division over before addition and subtraction, as in arithmetic.
+ Treat a carried-over piece as one value again, and put it where the left or right value of the outer operation goes.

For $(8 - 3) times (2 + 4)$ the left bracket is `8 3 sub`, the right one `2 4 add`, and `mul` after the two pieces
makes `8 3 sub 2 4 add mul`.

#ex("manual", "02-infix.pnx")

#trace(
  ("8 3", "8 3", [The two values of the left bracket.]),
  ("sub", "5", [$8 - 3$.]),
  ("2 4", "5 2 4", [The two values of the right bracket. `5` waits below.]),
  ("add", "5 6", [$2 + 4$.]),
  ("mul", "30", [Multiplies the results of the two brackets.]),
)

#qa[I lose track of which value goes where. Is there an easier way?][Compute one piece per line and give its result a
name (the locals of chapter 4). With `8 3 sub to left` you do not have to keep the stack in your head for long. Pinax
keeps the stack short and asks for names when it grows.]

== Words where order matters

Addition and multiplication give the same result whichever way round the two values are. Subtraction, division,
remainder and comparison do not. There is one rule: #strong[the value put on first is on the left]. `10 3 sub` is
$10 - 3$, and `3 10 lt` asks "is $3 < 10$?".

#ex("manual", "02-order.pnx")

`div` divides whole numbers and throws the remainder away; `mod` gives the remainder.

== Five words that rearrange the stack

Five words change only the shape of the stack and compute nothing. In the table `a` and `b` are any values, and the
right is the top.

#table(columns: (auto, auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Word*], [*Before*], [*After*], [*What it does*]),
  [`dup`], [`a`], [`a a`], [Makes one more of the top value.],
  [`drop`], [`a`], [`—`], [Throws the top value away.],
  [`swap`], [`a b`], [`b a`], [Exchanges the top two values.],
  [`over`], [`a b`], [`a b a`], [Puts one more of the second value on top.],
  [`nip`], [`a b`], [`b`], [Throws the second value away.],
)

#ex("manual", "02-stack-words.pnx")

Follow the `over` line. `over` #strong[copies] the value below, so `5` stays where it was. It has to be dropped after
the printing --- otherwise a value is left at the end of the program and the program is refused.

#trace(
  ("5 8", "5 8", [Two values go on.]),
  ("over", "5 8 5", [Copies the second value `5` to the top.]),
  ("sub", "5 3", [$8 - 5$; only the top two are used.]),
  ("str.to_str io.println", "5", [Prints `3`. `5` is left.]),
  ("drop", "—", [Throws the remaining `5` away.]),
)

#mistake[There is a `rot` that brings the third value to the top][There is not. Words that reach deep into the stack,
like Forth's `rot` or `pick`, are left out on purpose: once more than three values are in play the stack is hard to
follow in your head. Give the values names instead (named inputs in chapter 3, locals in chapter 4).
#ex("manual", "02-rot.pnx")]

== Types of values

#ex("manual", "02-values.pnx")

This book mostly uses four types:
- `int` --- 64-bit integers, written in decimal, hexadecimal (`0xff`) or binary (`0b101`), with underscores between
  digits if you like. A character `'A'` is the `u8` of its byte. `u8` `u16` `u32` `u64` `i8` `i16` `i32` are the
  integer types of those widths: `255 u8` makes one from an `int`, `raw` gives the `int` back, and none of them mixes
  with `int` in arithmetic.
- `bool` --- `true` and `false`; comparison words (`lt` `eq` …) give them and `if` takes them.
- `str` --- strings; chapter 5 covers them.
- `err` --- the reason for a failure; see chapter 8.

#qa[Do `int` and `bool` convert into each other?][No. `1` is not true, and you cannot add to `true`. Only a `bool`
goes where a condition goes, and only an `int` into arithmetic.]

== The processor knows the stack in advance

Before the program runs --- at load --- the processor already knows, at every place, how many values are on the stack
and the type of each. It draws the tables above for itself, in advance. So a mistake with the stack always shows before
the program runs, whether or not that line would ever run. There are three such mistakes.

First, #strong[too few values]. `add` needs two values and the stack holds one.

#ex("manual", "02-stack-short.pnx")

Second, #strong[the wrong type]. A string and an integer cannot be added.

#ex("manual", "02-types-mixed.pnx")

Third, #strong[values left over]. A value that nothing took when the program ends (and when the body of a word ends,
chapter 3) is refused: a left-over value is usually the trace of forgotten work.

#ex("manual", "02-leftover.pnx")

`5:1` in the message is the first column of the fifth line: where the source ends. Left-over values can only be known
at the end, so the end is pointed at.

#mistake[The depth of the stack may change as a program runs][Languages like Forth allow words that leave a different
number of values depending on what happens. Pinax does not. At every place the depth and types of the stack are one and
fixed. So the two branches of an `if` leave the same stack, and one turn of a loop leaves the stack as it was
(chapter 4). This rule is what makes the checks above possible.]

== Overflow is a fault

#ex("manual", "02-overflow.pnx")

A result beyond the range of `int` does not quietly wrap around. The run stops on the spot and the processor reports a
#strong[fault]. `pnx` shows the line of the fault and the locals of the word. For arithmetic that wraps use
`wrap_mul`, for arithmetic that stops at the edge `sat_mul`, and to receive overflow as a failure `chk_mul`.

A fault is not the same as a refusal at load. Whether a sum overflows depends on the values and cannot be known before
the run, so the run stops where it happens. Either way, memory is never damaged, and no wrong value goes quietly on.

== Decimals

For money and other numbers with a fixed number of digits after the point, write them with the point: `4.20` is a
`dec(2)`, a decimal with two digits after the point. It is held as the integer 420, so sums come out exactly, and the
same on every machine. Add and subtract it with the same `dec(N)`, multiply and divide it by an `int`; two `dec`
multiplied keep all their digits. `rescale` sets the number of digits, rounding half away from zero.

#ex("manual", "02-dec.pnx")

Dividing by an `int` cuts toward zero, as for integers: `9.75 3 div` is `3.25`, but `10.00 3 div` is `3.33`, and one cent
is left over. When that matters, take the cents with `raw` and share them out yourself.

#mistake[`4.20` and `0.5` can be added][They are `dec(2)` and `dec(1)`, two types. Write `0.50`, or `rescale` one.
#ex("manual", "02-dec-scales.pnx")]

#summary[
- Values go on the stack; a word takes values off the top, works, and puts its results on (standard 9.3).
- The value put on first is on the left: `10 3 sub` is 7 (standard 9.3).
- The stack words are `dup` `drop` `swap` `over` `nip` only; when values pile up, name them (9.3).
- The processor checks the depth and types of the stack at load: too few values, the wrong type and values left over
  are refused (7.1, 9.2, 9.3).
- A number with a point is a `dec(N)`: N digits after it, exact arithmetic, `rescale` to round (standard 7.5).
- Integer overflow and division by zero are faults; other arithmetic words exist (7.1, 15.1).
]

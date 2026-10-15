#import "/tools/common.typ": *

= Values and the stack <values>

#opening[Chapter 1 (the order of code, `pnx`).][
- What values does Pinax have?
- How does the stack move, and when does the processor check it?
- What happens when an integer overflows?
]

== Computing on the stack

#ex("manual", "02-stack.pnx")

Follow `2 3 add 4 mul` step by step. `2` and `3` go on; `add` takes both off and puts `5` on; `4` goes on; `mul` takes
both off and puts `20` on. Without parentheses, the order of the computation is the order of the text.

For words where order matters, such as subtraction and division, #strong[the value put on first is on the left]:
`10 3 sub` is $10 - 3$. `swap` exchanges the top two values. The only words that rearrange the stack are the five `dup`
`drop` `swap` `over` `nip`. When you need more, give the values names (chapter 4).

== Types of values

#ex("manual", "02-values.pnx")

This book mostly uses four types:
- `int` --- 64-bit integers, written in decimal, hexadecimal (`0xff`) or binary (`0b101`), with underscores between
  digits if you like. A character `'A'` is the `int` that is its number.
- `bool` --- `true` and `false`; comparison words (`lt` `eq` …) give them and `if` takes them.
- `str` --- strings; chapter 5 covers them.
- `err` --- the reason for a failure; see chapter 8.

#qa[Do `int` and `bool` convert into each other?][No. `1` is not true, and you cannot add to `true`. Only a `bool`
goes where a condition goes, and only an `int` into arithmetic.]

== The processor knows the stack in advance

At load the processor knows, at every place of a body, how many values are on the stack and of what types. So the two
programs below are refused before they run.

#ex("manual", "02-types-mixed.pnx")
#ex("manual", "02-stack-short.pnx")

#mistake[The depth of the stack may change as a program runs][Languages like Forth allow words that leave a different
number of values depending on what happens. Pinax does not. At every place the depth and types of the stack are one and
fixed. So the two branches of an `if` leave the same stack, and one turn of a loop leaves the stack as it was
(chapter 4).]

== Overflow is a fault

#ex("manual", "02-overflow.pnx")

A result beyond the range of `int` does not quietly wrap around. The run stops on the spot and the processor reports a
#strong[fault]. `pnx` shows the line of the fault and the locals of the word. For arithmetic that wraps use
`wrap_mul`, for arithmetic that stops at the edge `sat_mul`, and to receive overflow as a failure `chk_mul`.

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
- A number with a point is a `dec(N)`: N digits after it, exact arithmetic, `rescale` to round (standard 7.5).
- The value put on first is on the left: `10 3 sub` is 7 (standard 9.3).
- The stack words are `dup` `drop` `swap` `over` `nip` only (9.3).
- Types are checked at load; `int` and `bool` do not mix (7.1, 7.2).
- Integer overflow and division by zero are faults; other arithmetic words exist (7.1, 15.1).
]

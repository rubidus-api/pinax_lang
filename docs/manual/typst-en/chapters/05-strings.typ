#import "/tools/common.typ": *

= Strings <strings>

#opening[Chapter 2 (types of values), chapter 4 (stems and blocks).][
- Why does cutting and splitting a string copy nothing?
- How do `str` and `str(N)` differ, and when is each used?
- Where do the bytes of new text go, and how are they given back?
]

== Views: strings that do not copy

A `str` value is a #term("view"). It points at where some bytes are and how many; it does not own them. So cutting
(`str.take` `str.skip` `str.slice`), trimming (`str.trim`) and splitting (`str.cut` `str.split` `str.lines`
`str.words`) copy no bytes at all; they give new views of parts of the original text.

#ex("manual", "05-views.pnx")

This program splits settings text into lines, trims each line and cuts it in two at `=`. Bytes move only when they are
printed. `str.try_cut` fails when there is no `=`, so it is taken with `or_else`. `str.cut`, which does not fail, gives
an empty text after the separator when there is none.

== Stored text

A view cannot live longer than the bytes it points at. So places that live long --- globals, cells of arrays, fields of
records --- cannot hold views; they use #strong[stored text], `str(N)`. A `str(N)` takes N bytes of room and copies the
bytes put into it. Reading it gives a view of that room.

#ex("manual", "05-stored.pnx")
#ex("manual", "05-global-view.pnx")

#mistake[Text longer than `str(N)` is cut to fit][It is not. Putting text longer than the room stops with a fault. To
store part of it, cut it first with `str.take`.
#ex("manual", "05-too-long.pnx")]

== Making new text, and buffers

`cat` and words that make new bytes, such as `str.upper` and `str.repeat`, write those bytes into the #strong[region]
and give a view of them. To build text piece by piece, declare a #strong[buffer] and add to it with the `append` words.
When the buffer is full, the word that adds fails.

#ex("manual", "05-build.pnx")

== Giving the region back

The region has a fixed size for each run. A loop that keeps making text fills it up. `mark` remembers the current place
and `release` goes back to it; the bytes made in between are used again.

#ex("manual", "05-region.pnx")

After the release, views made in between cannot be used. The processor checks this at load too.

#ex("manual", "05-release-view.pnx")

#qa[How do I keep text that must outlive the release?][Put it into a `str(N)` global or cell. It is copied there, so it
survives rewinding the region. `05-stored.pnx` above does that.]

== Bytes and characters

Lengths and positions count bytes. A Hangul character is 3 bytes in UTF-8. When characters matter, use `str.char_count`
and `str.chars`. `str.chars` yields a view of each character (not a code point number).

#ex("manual", "05-chars.pnx")

#summary[
- `str` is a view; cutting and splitting copy nothing (standard 7.3, 16.1).
- Long-lived places use `str(N)`; putting text there copies it, and text too long is a fault (7.3).
- New bytes go into the region; `mark`/`release` gives them back, and views made in between are refused after
  (12.2, 12.3).
- A buffer is where text is built; it fails when full (8.7).
- Lengths are bytes; for characters, `char_count` and `chars` (16.1).
]

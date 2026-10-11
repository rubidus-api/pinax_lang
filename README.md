[한국어](README.ko.md) | **English** — **Pinax**

# Pinax

A small, fast scripting language to embed in C programs, with fixed, flat memory.

> **Status: design stage.** There is no implementation yet. This page states what the
> project is aiming for; nothing below can be built or run today.

Pinax is meant to live inside a host program. The host hands it one block of memory,
registers the functions it may call, and runs scripts in it. Every value has a type and a
place decided when a script is loaded, so the interpreter needs no heap, no garbage
collector and no pointers at run time.

## Goals

- **Easy to embed.** One state object, one memory block, no global state. Linking with C
  and calling existing C libraries comes first in the design.
- **Fast integers.** 64-bit integers with defined overflow behaviour and a full set of bit
  operations.
- **Fast strings.** Slicing, splitting and searching produce views, not copies.
- **Fast serialization.** State is flat memory, so it can be saved and restored as it is;
  records are packed and unpacked by code generated from their declaration.
- **Predictable.** Capacities are declared up front and checked at load. Running out of
  room is an error value, not a crash, and there are no collection pauses.

## Not goals

Systems programming, garbage collection, general heap allocation, object orientation,
dynamic typing.

## The name

*Pinax* (Greek *pinax*, also used in Latin) is a flat board or writing tablet, and by
extension a catalogue. The language keeps its data flat.

## Stack

Pure C23. The only dependency will be a vendored copy of
[proven_c_lib](https://github.com/rubidus-api/proven_c_lib) (MIT).

## License

MIT. See [LICENSE](LICENSE).

#import "/tools/common.typ": *

== `hash` --- hashes <std-hash>

#para("1")[The module `hash` gives hashes of bytes as an `int`. There are no cryptographic hashes; a host gives those as
host words.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`crc32`], [`(str gives int)`], [CRC-32 (IEEE 802.3, as zlib), from 0 to 2#super[32]−1],
  [`fnv1a`], [`(str gives int)`], [64-bit FNV-1a],
  [`hash64`], [`(str gives int)`], [SipHash-2-4 with the key 0],
)]

#ex("spec", "16-hash.pnx")

#para("1a")[Check values: the `crc32` of the empty string is 0, and that of `"123456789"` is `cbf43926` (the standard
check value of CRC-32). The `fnv1a` of the empty string is FNV's offset basis `cbf29ce484222325`. Another processor
shall give the same values.]
#ex("spec", "16-hash-vectors.pnx")

#part("Constraints")

#para("2")[The hash of a value that is not a `str` is refused. To hash a number, make bytes of it first (16.5).]
#ex("spec", "16-hash-type.pnx")

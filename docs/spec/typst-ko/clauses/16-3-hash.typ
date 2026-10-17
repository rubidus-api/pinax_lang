#import "/tools/common.typ": *

== `hash` --- 해시 <std-hash>

#para("1")[모듈 `hash` 는 바이트의 해시를 `int` 로 준다. 암호에 쓰는 해시는 없다 --- 그런 것은 호스트 낱말로
준다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`crc32`], [`(str gives int)`], [CRC-32 (IEEE 802.3, zlib 과 같음). 0 부터 2#super[32]−1],
  [`fnv1a`], [`(str gives int)`], [64 비트 FNV-1a],
  [`hash64`], [`(str gives int)`], [키가 0 인 SipHash-2-4],
)]

#ex("spec", "16-hash.pnx")

#para("1a")[검사 값: 빈 글의 `crc32` 는 0 이고, `"123456789"` 의 `crc32` 는 `cbf43926` 이다(CRC-32 의 표준 검사 값). 빈 글의
`fnv1a` 는 FNV 의 시작값 `cbf29ce484222325` 다. 다른 처리기는 같은 값을 내야 한다.]
#ex("spec", "16-hash-vectors.pnx")

#part("제약")

#para("2")[`str` 가 아닌 값의 해시는 거절된다. 수의 해시는 먼저 바이트로 만든다(16.5).]
#ex("spec", "16-hash-type.pnx")

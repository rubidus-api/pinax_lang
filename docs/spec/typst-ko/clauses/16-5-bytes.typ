#import "/tools/common.typ": *

== `bytes` --- 이진 자료 <std-bytes>

#para("1")[모듈 `bytes` 는 수와 글을 정해진 바이트 모양으로 버퍼에 쓰고, 바이트에서 읽는다. 쓰는 낱말은
`(값, buf or fail)` 이고(자리가 모자라면 `err.full`, 아무것도 쓰지 않는다), 읽는 낱말은 `(s str gives 값, 나머지
str or fail)` 이다(바이트가 모자라거나 모양이 틀리면 `err.format`).]

#para("2")[
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*바이트*],
  [`put_u8` `put_u16` `put_u32` `put_u64` \ `put_i8` `put_i16` `put_i32` `put_i64`], [이름이 말하는 타입의 값(7.6; `put_i64` 는 `int`). 작은 끝이 먼저],
  [같은 이름 뒤에 `_be`], [큰 끝이 먼저],
  [`get_u8` … `get_i64`, `get_u16_be` …], [위의 짝. 이름이 말하는 타입의 값을 준다(`get_i64` 는 `int`)],
  [`put_varint` `get_varint`], [LEB128. 값을 부호 없는 64 비트로 보고 7 비트씩 쓴다(음수는 10 바이트)],
  [`put_str` `get_str`], [길이(varint) 다음에 바이트],
)]

#ex("spec", "16-bytes.pnx")

=== 레코드 <std-bytes-records>

#para("3")[`줄 buf bytes.pack` 은 레코드(8.8)의 줄 하나를 필드의 차례대로 버퍼에 붙인다(`or fail`).
`s 줄 bytes.unpack` 은 바이트에서 필드를 읽어 줄에 넣고 나머지 뷰를 준다(`gives str or fail`).
`줄 bytes.packed_size` 는 `pack` 이 쓸 바이트 수다.]

#para("4")[필드의 바이트: 정수 폭은 그 폭으로(레코드에 `big_endian` 이 있으면 큰 끝이 먼저, 없으면 작은 끝이
먼저), `int` 는 8 바이트, `bool` 은 1 바이트, `varint` 는 LEB128, `str(N)` 은 길이(varint) 다음에 바이트, 줄을
담는 필드는 담긴 수(varint).]

#para("5")[`pack` 이 실패하면 버퍼는 부르기 전의 길이로 돌아간다. `unpack` 은 모든 필드를 읽을 수 있을 때만 줄에
쓴다 --- 실패하면 줄은 그대로다.]

#ex("spec", "16-bytes-pack.pnx")

#part("제약")

#para("6")[`pack` `unpack` `packed_size` 는 레코드의 줄만 받는다.]
#ex("spec", "16-bytes-pack-int.pnx")

#part("결함")

#para("7")[쓰는 값은 낱말 이름이 말하는 타입이다(`put_u8` 이면 `u8`, 7.6). `int` 는 먼저 그 타입으로 바꾸고(`n u8`), 범위
밖의 값은 거기서 결함 `overflow` 다. 자르지 않는다.]
#ex("spec", "16-bytes-narrow.pnx")

#import "/tools/common.typ": *

== `str` --- 문자열 <std-str>

#para("1")[모듈 `str` 은 문자열을 바이트로 다룬다. 바이트의 번호는 0 부터 센다. 글자 단위로 다루는 낱말
(`chars` `char_count` `is_utf8`)만 UTF-8 로 읽는다. 대소문자 바꾸기(`upper` `lower`)는 ASCII 만 바꾼다.]

#para("2")[구분자나 찾는 것을 받는 자리(`sep`, `part`)에는 `str` 이나 문자 하나(`int`, 6.5)를 줄 수 있다.]

=== 보기와 자르기

#para("3")[결과는 모두 받은 문자열의 뷰다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`len` `is_empty`], [`(str gives int)` `(str gives bool)`], [바이트 수, 비었는가],
  [`take` `skip`], [`(s str, n int gives str)`], [앞의 `n` 바이트, 앞의 `n` 바이트를 뺀 나머지. 모자라면 있는 만큼],
  [`take_last` `skip_last`], [`(s str, n int gives str)`], [뒤의 `n` 바이트, 뒤의 `n` 바이트를 뺀 앞],
  [`slice`], [`(s str, start int, n int gives str)`], [`start` 부터 `n` 바이트. 범위 밖이면 결함 `range`],
  [`trim` `trim_left` `trim_right`], [`(str gives str)`], [앞뒤의 공백(스페이스, 탭, CR, LF)을 뺀 뷰],
  [`cut`], [`(s str, sep gives str, str)`], [처음 `sep` 의 앞과 뒤. `sep` 이 없으면 `s` 와 빈 글],
  [`try_cut`], [`(s str, sep gives str, str or fail)`], [`cut` 과 같으나 `sep` 이 없으면 `err.not_found`],
  [`span`], [`(s str, class int gives str, str)`], [앞에서 그 갈래의 바이트가 이어진 부분과 나머지],
)]

#para("4")[`span` 의 갈래는 상수 `str.digit` `str.alpha` `str.alnum` `str.space` `str.upper_case`
`str.lower_case` `str.hex_digit` `str.punct` 다(ASCII).]

#ex("spec", "16-str-views.pnx")

=== 찾기와 견주기

#para("5")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`starts_with` `ends_with` `contains`], [`(s str, part gives bool)`], [],
  [`find` `find_last`], [`(s str, part gives int or fail)`], [처음, 마지막으로 나온 자리. 없으면 `err.not_found`],
  [`compare`], [`(str, str gives int)`], [바이트 차례로 견주어 음수, 0, 양수],
  [`byte_at`], [`(s str, i int gives int)`], [바이트 `i`. 범위 밖이면 결함 `range`],
  [`char_count`], [`(str gives int)`], [UTF-8 글자의 수],
  [`is_utf8`], [`(str gives bool)`], [올바른 UTF-8 인가],
)
`eq` `ne` `lt` 등 비교 낱말도 문자열을 바이트 차례로 견준다(15.2).]

#ex("spec", "16-str-search.pnx")

=== 새 글 만들기

#para("6")[결과는 구역에 쓴 새 바이트다. 구역이 모자라면 결함 `full` 이다(12.2). 수로 읽는 낱말은 실패할 수 있다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`upper` `lower`], [`(str gives str)`], [ASCII 대소문자 바꾸기],
  [`repeat`], [`(s str, n int gives str)`], [`n` 번 이은 글. `n` 이 음수면 결함 `range`],
  [`replace`], [`(s str, old str, new str gives str)`], [모든 `old` 를 `new` 로],
  [`pad_left` `pad_right`], [`(s str, width int gives str)`], [스페이스로 채워 `width` 바이트로. 이미 길면 그대로. `width` 가 음수이거나 2#super[32] 이상이면 결함 `range`],
  [`to_str`], [`(int gives str)`], [십진 글. `dec(N)` 은 점 아래 `N` 자리까지(7.5)],
  [`to_dec`], [`(s str, N gives dec(N) or fail)`], [십진 소수로 읽기: 부호, 숫자, 점과 그 아래 `N` 자리까지(`N` 은 상수). 자리가 더 많거나 다른 꼴이면 `err.format`],
  [`to_int`], [`(str gives int or fail)`], [십진으로 읽기. 부호 하나와 숫자만 받는다(공백도 안 된다). 아니거나 64 비트를 넘으면 `err.format`],
  [`to_int_base`], [`(s str, base int gives int or fail)`], [2--36 진법으로 읽기. 그 밖의 `base` 는 결함 `range`],
)]

#ex("spec", "16-str-make.pnx")

=== 줄기와 버퍼에 쓰기

#para("7")[줄기(10.6)를 내는 낱말:
- `s sep str.split` --- `sep` 으로 가른 조각. 빈 조각도 낸다.
- `s str.words` --- 공백으로 가른 조각. 빈 조각은 내지 않는다.
- `s str.lines` --- 줄. 줄 끝의 LF 와 CR LF 는 빼고 낸다.
- `s str.bytes` --- 바이트(`int`).
- `s str.chars` --- UTF-8 글자 하나씩의 뷰(`str`).]

#para("8")[버퍼에 쓰는 낱말:
- `sep buf 줄기 str.join_into` --- 줄기의 조각을 `sep` 으로 이어 버퍼에 붙인다(`or fail`). 줄기는 바로 뒤의
  낱말이 받아야 하므로 줄기를 맨 뒤에 둔다.
- `s old new buf str.replace_into` --- 바꾼 글을 버퍼에 붙이고 바꾼 횟수를 준다(`gives n int or fail`).]

#ex("spec", "16-str-stems.pnx")

#part("제약")

#para("9")[`str` 의 낱말에 `str` 가 아닌 값을 주면 거절된다.]
#ex("spec", "16-str-type.pnx")

#part("결함")

#para("10")[`slice` 와 `byte_at` 의 범위 밖 번호는 결함이다. 모자라도 되는 자르기는 `take` `skip` 을 쓴다.]
#ex("spec", "16-str-slice-range.pnx")
#ex("spec", "16-str-byte-range.pnx")

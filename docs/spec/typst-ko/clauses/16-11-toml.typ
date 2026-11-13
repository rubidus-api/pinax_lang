#import "/tools/common.typ": *

== `toml` --- TOML <std-toml>

#para("1")[모듈 `toml` 은 설정 파일인 TOML 버전 1 글을 나무로 만들지 않고 읽는다. 값은 글에 적힌 바이트 그대로 주고, 문자열은 따옴표를 뗀다. 이스케이프가 있는
문자열, 또는 줄 끝 역슬래시가 있는 여러 줄 문자열은 풀어서 구역(12.2)에 쓴 글로 준다. 아무것도 바꾸지 않는다: 수는 적힌 그대로(`5_000`, `0xff`) 주므로,
프로그램이 원하는 꼴의 말로 읽는다. 이 모듈은 읽기만 하고 TOML 을 쓰지 않는다.]

#para("2")[`kind` 는 아래 상수다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*상수*], [*값*],
  [`toml.table`], [머리 `[a.b]`, 또는 경로가 가리키는 표: 값 없음],
  [`toml.tables`], [표 배열의 머리 `[[a.b]]`: 값 없음],
  [`toml.string`], [네 가지 문자열 중 어느 것이든],
  [`toml.int` `toml.float`], [적힌 그대로의 수. `inf` 와 `nan` 은 float],
  [`toml.bool`], [`true` 또는 `false`],
  [`toml.datetime`], [날짜, 시각, 또는 둘, 적힌 그대로(`1979-05-27T07:32:00Z`)],
  [`toml.array` `toml.inline`], [배열이나 인라인 표를 적힌 대로 통째로. `toml.at` 으로 다시 읽을 수 있다],
)]

=== 읽기

#para("3")[`s toml.next` 는 `s` 의 첫 문장을 읽어 `(kind int, key str, value str, rest str)` 를 준다(`or fail`): 머리이면(`kind` 는 `toml.table` 또는
`toml.tables`, `key` 는 괄호 사이의 이름 그대로, `value` 는 비었다), 쌍이면(`key` 는 적힌 대로 점과 따옴표까지, `value` 는 위와 같다). 빈 줄과 주석은 건너뛴다.
끝에 이르면 `err.not_found`, TOML 이 아닌 문장은 `err.format` 으로 실패한다. 프로그램이 지금 어느 머리 아래인지는 스스로 기억한다. 모든 `key` 는 그 머리에 상대적이다.]

#para("4")[`path s toml.at` 은 `(kind int, value str)` 를 준다(`or fail`): 파일을 읽는 사람이 보는 대로 `path` 가 가리키는 값. 경로는 점으로 이은 이름(따옴표를
친 이름 `"a.b"` `'a.b'` 에는 점이 들 수 있다)과 괄호 안의 번호다: `server.ports[1]`, `user[0].name`, `a."b c".d`. 점을 쓴 키(`a.b.c = 1`), 머리가 만든 표,
인라인 표, 배열을 모두 따라가고, 번호는 값 배열이나 표 배열(`[[...]]` 머리의 차례, 한 원소 아래의 것은 그 원소에 속한다)의 원소를 고른다. 표를 가리키는
경로는 `toml.table` 과 값 없음을 준다. 없으면 `err.not_found`, 경로가 잘못됐거나 읽은 곳의 글이 잘못됐으면 `err.format`.]

#para("5")[`s toml.valid` 는 `s` 의 모든 문장이 꼴이 맞는지 준다. 검사는 문장 하나하나의 문법이다: 두 번 준 키, 두 번 정의한 표, 수의 범위, 있는 날짜인지는 검사하지 않는다.]

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*말*], [*서명*], [*뜻*],
  [`valid`], [`(s str gives bool)`], [`s` 의 모든 문장이 꼴이 맞는지],
  [`at`], [`(path str, s str gives kind int, value str or fail)`], [`s` 에서 `path` 가 가리키는 값. `err.not_found`, `err.format`],
  [`next`], [`(s str gives kind int, key str, value str, rest str or fail)`], [`s` 의 첫 문장과 그 뒤의 나머지. 끝에서 `err.not_found`, `err.format`],
)

#ex("spec", "16-toml.pnx")

#part("제약")

#para("6")[인자는 `str` 이다. 경로는 40 걸음, 키는 16 토막, 서로 안에 든 배열과 인라인 표는 32 단까지다. 표 배열이 서로 다른 것으로 32 개를 넘는 글은 `toml.at` 에서 `err.format` 이다.]
#ex("spec", "16-toml-type.pnx")

#part("진단")
#diag(
  ([`toml` 의 말에 다른 형의 값], "this word takes a str (the text, and the path before it)"),
)

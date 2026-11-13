#import "/tools/common.typ": *

== `json` --- JSON <std-json>

#para("1")[모듈 `json` 은 JSON(RFC 8259) 글을 나무로 만들지 않고 한 방향으로 읽고 쓴다. 읽기는 토큰 하나씩,
쓰기는 버퍼에 바로 한다.]

=== 읽기

#para("2")[`s json.next` 는 `s` 의 첫 토큰을 읽어 `(kind int, value str, rest str)` 를 준다(`or fail`). 끝에
이르면 `err.not_found`, 토큰이 아닌 것을 만나면 `err.format` 으로 실패한다. 토큰 사이의 공백, 쉼표, 쌍점은
건너뛴다. 괄호의 짝이나 쉼표의 자리 같은 문법의 짜임은 검사하지 않는다.]

#para("3")[`kind` 는 아래 상수다. 문자열의 `value` 는 따옴표를 뗀 글이고, 이스케이프가 있으면 풀어서 구역에 쓴
글이다. 수의 `value` 는 적힌 그대로의 글이다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*상수*], [*토큰*],
  [`json.object` `json.object_end`], [`{` `}`],
  [`json.array` `json.array_end`], [`[` `]`],
  [`json.key`], [쌍점이 뒤따르는 문자열],
  [`json.string`], [그 밖의 문자열],
  [`json.number`], [수],
  [`json.boolean` `json.null`], [`true` `false`, `null`],
)]

#para("4")[`s json.skip` 은 값 하나(객체나 배열이면 그 안 전부)를 건너뛰고 나머지를 준다(`gives str or fail`).]

#ex("spec", "16-json-read.pnx")

=== 쓰기

#para("5")[쓰는 낱말은 모두 버퍼에 붙이고, 자리가 모자라면 `err.full` 로 실패한다. 쉼표는 낱말이 알아서 넣는다.
#table(columns: (auto, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*],
  [`begin_object` `end_object` `begin_array` `end_array` `put_null`], [`(buf or fail)`],
  [`put_key` `put_str`], [`(s str, buf or fail)`. 이스케이프는 낱말이 한다],
  [`put_int`], [`(n int, buf or fail)`],
  [`put_dec`], [`(d dec(N), buf or fail)`],
  [`put_bool`], [`(b bool, buf or fail)`],
)]

#ex("spec", "16-json-write.pnx")

=== 경로, 검증, 모양

#para("5a")[말 넷이 글 전체를 RFC 8259 의 엄격한 문법으로 읽는다(위의 토큰 읽기는 흐름을 읽으려고 일부러 느슨하다). 글이 값 하나와 공백뿐이면
유효하다. 64 단보다 깊은 중첩은 여기서 유효하지 않다. 경로는 값을 가리킨다: 점으로 이은 이름, 괄호 안의 번호, 점이나 괄호가 든 이름은 `["a.b"]`
(`user.langs[1]`, `[0].id`). 빈 경로는 글 전체다. 이름은 키의 이스케이프를 푼 글과 견준다. 값을 찾으면 거기서 읽기를 멈추므로, 나머지 글은 검사하지 않는다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*말*], [*서명*], [*뜻*],
  [`valid`], [`(s str gives bool)`], [`s` 가 JSON 값 하나와 그 둘레의 공백뿐인지],
  [`at`], [`(path str, s str gives kind int, value str or fail)`], [`s` 에서 `path` 가 가리키는 값: 종류(위의 상수, `json.key` 는 없다)와 글. 글은 `json.next` 가 주는 그대로이되, 객체와 배열은 적힌 대로 통째로 주어 `json.next` 나 `json.at` 으로 다시 읽을 수 있다. 그런 값이 없으면(없는 이름, 끝을 넘은 번호, 배열에 이름을 묻거나 객체에 번호를 묻기) `err.not_found`, 경로가 잘못됐거나 읽은 곳의 글이 잘못됐으면 `err.format`],
  [`minify`], [`(s str, b buffer or fail)`], [토큰 사이의 공백을 뺀 `s` 를 덧붙인다. `s` 가 유효하지 않으면 `err.format`, 자리가 모자라면 `err.full`(버퍼는 그대로)],
  [`pretty`], [`(s str, indent int, b buffer or fail)`], [멤버와 요소마다 한 줄, 한 단에 `indent` 칸(0에서 16, 그 밖은 결함), 키 뒤에 `": "` 로 덧붙인다. 빈 객체와 배열은 그대로 둔다. 실패는 `minify` 와 같다],
)]
#ex("spec", "16-json-path.pnx")

=== 레코드

#para("6")[`줄 buf json.pack` 은 레코드의 줄을 필드 이름을 키로 한 객체로 붙인다. `s 줄 json.unpack` 은 객체를
읽어 이름이 같은 필드에 넣고 객체 뒤의 나머지를 준다. 모르는 키의 값은 건너뛴다. 수는 정수만 받고, 필드의
폭에 맞아야 한다. `dec(N)` 필드는 점 아래 `N` 자리까지의 수를 받고, `N` 자리를 모두 써서 적는다(7.5). `null` 은
받지 않는다. 모든 것이 맞을 때만 줄에 쓴다 --- 실패하면(`err.format`) 줄은 그대로다.]

#ex("spec", "16-json-record.pnx")
#ex("spec", "16-dec-text.pnx")

#part("제약")

#para("7")[`json.next` 와 `json.skip` 은 `str` 만, `pack` 과 `unpack` 은 레코드의 줄만 받는다. `minify`, `pretty`, `valid`, `at` 의 원문은 그들이 쓰는 버퍼가 아니다.]
#ex("spec", "16-json-type.pnx")

#part("진단")
#diag(
  ([JSON 글이 `str` 이 아님], "this word takes the JSON text as a str"),
)

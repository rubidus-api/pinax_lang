#import "/tools/common.typ": *

== `md` --- Markdown <std-md>

#para("1")[모듈 `md` 는 사람이 쓰는 글인 Markdown 을 읽어 HTML 로 쓴다. CommonMark 의 부분집합에 GFM 의 표와 취소선을 더한 것으로, 스크립트가 만나는 메모, README,
쪽에 맞춘 것이지 규격의 구석구석까지 따르지 않는다. 글 속의 날 HTML 은 절대 넘기지 않는다: `<` 는 `&lt;` 로 쓴다. 주소의 체계가 `http`, `https`, `mailto`, `tel`,
`ftp` 가 아닌 링크와 그림은 글만 쓴다. 그래서 Markdown 이 무엇이든 그 HTML 은 쪽에 넣어도 안전하다.]

#para("2")[읽는 것. 블록: ATX 제목(`#` 에서 `######`)과 밑줄 제목, 문단, 가로줄, 인용, 목록(`-` `+` `*` 글머리표, `1.` `1)` 번호, 중첩, 촘촘한 것과 성긴 것), 울타리
코드(백틱이나 물결표 셋 이상, 정보 문자열의 첫 낱말이 글자, 숫자, `- _ + # .` 이면 `class="language-..."` 가 된다), 들여쓴 코드, 표(한 줄, 정렬을 콜론으로 적은
줄, 줄들). 인라인: 강조 `*a*` `_a_`, 굵게 `**a**` `__a__`, 취소선 `~~a~~`, 코드, 링크 `[글](주소 "제목")`, 그림 `![대체글](주소)`, 자동 링크 `<https://...>` 와
`<a@b.c>`, 역슬래시 이스케이프, 문자 참조(`&amp;` `&#65;`), 강제 줄바꿈(줄 끝 공백 둘 또는 역슬래시). 읽지 않는 것: 링크 참조 정의, 날 HTML, 목록 뒤의 밑줄 제목,
구두점 곁 강조의 CommonMark 세부 규칙. 이들은 글이나 보통 문단이 될 뿐 실패하지 않는다. 12 단보다 깊은 컨테이너나 강조의 중첩은 글로 쓴다. 링크, 강조, 코드의 닫는 표를 찾는 일은 글의 바이트마다 16 바이트의 예산으로 치르고, 예산이 다하면 남은 표는 글이 된다.
그래서 닫히지 않은 표뿐인 글도 크기에 비례하는 시간에 읽는다.]

=== HTML 쓰기

#para("3")[`s b md.html` 은 Markdown 글 `s` 의 HTML 을 버퍼 `b` 에 덧붙이고, `s b md.inline` 은 블록 없이 한 줄이나 한 문단만의 HTML 을 덧붙인다. 자리가 모자라면
`err.full` 로 실패한다(버퍼는 그대로). 자리는 HTML 자체에 더해, 인용이나 목록 항목을 쓰는 동안 그 복사본을 버퍼 끝에 둘 만큼, 곧 블록이 든 단마다 글보다 길지 않은 복사본 하나씩의 바이트가 더 든다.
`s` 는 그 버퍼가 아니다.]

=== 블록으로 읽기

#para("4")[`s md.next` 는 `s` 의 첫 블록을 읽어 `(kind int, level int, text str, rest str)` 를 준다(`or fail`). 끝에 이르면 `err.not_found` 로 실패한다. 아무것도 복사하지
않는다: `text` 는 `s` 의 뷰다. `kind` 는 아래 상수, `text` 는 표가 말하는 것, `level` 은 제목이면 그 단계(1에서 6), 번호 목록이면 1, 그 밖에는 0 이다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*상수*], [*text*],
  [`md.heading`], [`#` 와 밑줄을 뗀 제목의 글. 인라인 Markdown 으로 읽는다],
  [`md.paragraph`], [문단의 줄들, 적힌 대로],
  [`md.code`], [울타리 코드: 울타리 사이의 줄들. 들여쓴 코드: 들여쓰기를 그대로 둔 줄들],
  [`md.quote` `md.list` `md.table`], [표지를 그대로 둔 블록. `md.html` 에 준다],
  [`md.rule`], [빈 글],
)]

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*말*], [*서명*], [*뜻*],
  [`html`], [`(s str, b buffer or fail)`], [Markdown 글 `s` 의 HTML 을 덧붙인다. `err.full`],
  [`inline`], [`(s str, b buffer or fail)`], [한 줄이나 한 문단의 글의 HTML 을 덧붙인다. `err.full`],
  [`next`], [`(s str gives kind int, level int, text str, rest str or fail)`], [`s` 의 첫 블록과 그 뒤의 나머지. 끝에서 `err.not_found`],
)

#ex("spec", "16-md.pnx")

#part("제약")

#para("5")[`s` 는 `str` 이고, `html` 과 `inline` 의 버퍼는 맨 뒤에 온다.]
#ex("spec", "16-md-type.pnx")

#part("진단")
#diag(
  ([`md` 의 말에 다른 형의 값], "this word takes a str (the text, and the path before it)"),
)

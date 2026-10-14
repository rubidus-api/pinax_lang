#import "/tools/common.typ": *
// shape: host words follow the rules of clauses 9 to 12; their refusals are shown there (12-owned-*)

= 표준 호스트 모듈 <host-modules>

#para("1")[이 절의 모듈은 처리기가 함께 내는 호스트 낱말이다. 언어의 일부가 아니다 --- 호스트가 등록할 때만
있다(13.3). `pnx` 는 다섯 모두를 등록한다. 라이브러리를 끼워 쓰는 호스트는 `pinax_host_register(state,
"io")` 처럼 모듈 하나씩 고른다(라이브러리 `libpinax_host`). 등록하지 않은 모듈을 `use` 하면 거절된다(13.1).]

#para("2")[호스트 낱말도 다른 낱말과 같은 규칙을 따른다: 서명은 적재 때 검사되고, `or fail` 이 있는 낱말은
지켜야 하며, `owned` 값은 정확히 한 번 넘긴다.]

== `io` --- 표준 입출력 <host-io>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`print` `println`], [`(s str)`], [표준 출력에 쓴다. `println` 은 줄바꿈(LF)을 붙인다],
  [`eprint`], [`(s str)`], [표준 오류에 쓴다],
  [`read_line`], [`(gives line str or fail)`], [표준 입력의 한 줄. 줄 끝은 뗀다. 입력이 끝나면 `err.not_found`],
  [`read_line_into`], [`(buf buffer or fail)`], [한 줄을 버퍼에 붙인다],
)
바이트는 고치지 않고 쓴다. 모든 운영체제에서 같은 바이트가 나간다.]

#ex("spec", "17-io.pnx")

== `file` --- 파일 <host-file>

#para("1")[핸들 타입 `file` 은 열린 파일이다. 여는 낱말이 `owned file` 을 주고, `close` 가 받는다(12.4).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`open_read` `open_write`], [`(path str gives owned file or fail)`], [읽기로, 쓰기로(새로 만들거나 비우고) 연다],
  [`write`], [`(s str, f file or fail)`], [바이트를 쓴다],
  [`read_line`], [`(f file gives line str or fail)`], [한 줄. 끝이면 `err.not_found`],
  [`read`], [`(f file, buf buffer gives n int or fail)`], [버퍼에 읽어 붙이고 읽은 바이트 수를 준다],
  [`read_line_into`], [`(f file, buf buffer or fail)`], [한 줄을 버퍼에 붙인다],
  [`read_all`], [`(path str gives body str or fail)`], [파일 전체를 구역에 읽는다],
  [`read_all_into`], [`(path str, buf buffer or fail)`], [파일 전체를 버퍼에 붙인다],
  [`close`], [`(f owned file)`], [닫는다],
)
없는 파일은 `err.not_found` 다.]

#ex("spec", "17-file.pnx")

== `clock` `random` `env` <host-misc>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`clock.now_ms`], [`(gives int)`], [지금 시각, 1970-01-01 UTC 부터의 밀리초],
  [`clock.monotonic_ns`], [`(gives int)`], [거꾸로 가지 않는 시계의 나노초],
  [`clock.sleep`], [`(ms int)`], [멈춰 기다린다. 그동안 상태는 다른 일을 하지 않는다],
  [`random.seed`], [`(n int)`], [수열을 정한다. 같은 씨앗은 같은 수열을 낸다],
  [`random.random`], [`(gives int)`], [64 비트 난수(암호용이 아님)],
  [`random.random_below`], [`(n int gives int)`], [0 부터 `n`−1 까지],
  [`env.arg_count`], [`(gives int)`], [인자의 수. 0 번은 프로그램 파일],
  [`env.arg`], [`(i int gives s str or fail)`], [인자 `i`],
  [`env.getenv`], [`(name str gives s str or fail)`], [환경 변수. 없으면 `err.not_found`],
)]

#ex("spec", "17-env.pnx")
#ex("spec", "17-random-clock.pnx")

#import "/tools/common.typ": *
// shape: host words follow the rules of clauses 9 to 12; their refusals are shown there (12-owned-*)

= 표준 호스트 모듈 <host-modules>

#para("1")[이 절의 모듈은 처리기가 함께 내는 호스트 낱말이다. 언어의 일부가 아니다 --- 호스트가 등록할 때만
있다(13.3). `pnx` 는 다섯 모두를 등록한다. 라이브러리를 끼워 쓰는 호스트는 `pinax_host_register(state,
"io")` 처럼 모듈 하나씩 고른다(라이브러리 `libpinax_host`). 등록하지 않은 모듈을 `use` 하면 거절된다(13.1).]

#para("2")[호스트 낱말도 다른 낱말과 같은 규칙을 따른다: 서명은 적재 때 검사되고, `or fail` 이 있는 낱말은
지켜야 하며, `owned` 값은 정확히 한 번 넘긴다.]

#para("3")[운영체제가 거절한 일은 실패(`or fail`)다. 없는 파일과 입력의 끝은 `err.not_found`, 바이트가 모양에 맞지 않으면
`err.format`, 자리가 모자라면 `err.full`, 그 밖의 거절(권한, 끊긴 장치)은 `err.failed` 다. `or fail` 이 없는 낱말(`print`,
`close`)은 운영체제의 거절을 알리지 않는다.]

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

== 채널과 호스트 루프 <host-channel>

#para("1")[#term("채널", en: "channel")은 바이트 메시지를 정한 수까지 담는 줄이다. 호스트가 채널에 이름을 짓고 상태에
그 이름의 모듈로 등록한다(`pinax_channel_register`, 라이브러리 `libpinax_host`). 같은 채널을 등록한 상태들이
메시지를 주고받는다. 상태끼리 함께 쓰는 메모리는 없다: 메시지는 들어갈 때와 나올 때 복사된다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`send`], [`(msg str or fail)`], [메시지를 넣는다. 채널이 차 있으면 기다린다],
  [`receive`], [`(gives msg str or fail)`], [가장 먼저 들어온 메시지를 꺼낸다. 비어 있으면 기다린다],
  [`close`], [`()`], [더는 넣지 못하게 한다],
)]

#para("2")[`send` 는 메시지가 채널이 정한 길이보다 길면 `err.full`, 채널이 닫힌 뒤에는 `err.cancelled` 로
실패한다. `receive` 는 닫히고 빈 채널에서 `err.not_found` 로 실패한다. 닫힌 채널에 남은 메시지는 그대로 꺼낸다.]

#para("3")[기다리는 것은 부른 작업뿐이다(14.2). 같은 상태의 다른 작업은 계속 돈다. 기다릴 수 있는 것은 호스트
루프(`pinax_loop_start`)가 돌리는 실행뿐이다. 다른 실행에서 기다려야 하는 `send` 나 `receive` 는 결함
`PINAX_ERR_STATE` 로 실행을 끝낸다.]

#para("4")[#term("호스트 루프", en: "host loop")는 상태들을 정한 수의 스레드에서 돌린다. `pinax_loop_open` 은 설정을 받는다:
스레드의 수, 한 번에 쥐는 실행의 수, 호스트 일의 결과가 차지할 수 있는 크기, 그리고 상태가 받듯 메모리 블록 하나.
루프의 객체는 그 안의 고정 칸이고(`pinax_loop_size` 바이트), 채널은 열릴 때 같은 블록에서 자기 칸을 잘라 간다
(`pinax_channel_size` 바이트). 실행이 도는 동안 할당은 없다. 블록이 없으면 루프가 그 크기의 조각 하나를 proven 의 힙에서
받는다.
한 상태는 한 번에 한 스레드에서만 돈다. 예산(19.5)이 다한 실행은 줄의 뒤로 가고, 기다리는 실행은 스레드를 내놓는다. 호스트가 `pinax_loop_wait`
로 기다리는 동안 남은 실행이 모두 채널을 기다리고 끝나지 않은 호스트 일이 없으면, 루프는 그 실행을 모두
`PINAX_ERR_DEADLOCK` 으로 끝낸다.]

#ex("spec", "17-channel.c")

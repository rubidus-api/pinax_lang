#import "/tools/common.typ": *

= 한 번만 쓰는 값과 구역 <owned>

#opening[5장(구역, `mark`/`release`), 7장(풀의 줄), 8장(실패).][
- 열린 파일처럼 꼭 닫아야 하는 것을 처리기는 어떻게 지켜 주는가?
- 실패하는 경로에서도 닫았는지 어떻게 아는가?
- 글을 많이 만드는 루프는 구역을 어떻게 아껴 쓰는가?
- 닫는 코드를 떠나는 길마다 되풀이하지 않으려면?
]

== 넘겨주어야 하는 값

`file.open_read` 의 서명은 `(path str gives owned file or fail)` 이다. `owned` 는 이 값이 #strong[한 번만 쓰는 값]이라는
뜻이다. 받은 쪽은 그 값을 모든 경로에서 정확히 한 번 #strong[넘겨줘야] 한다: `owned` 로 받는 낱말(`file.close`)에 주거나,
전역에 넣거나, `owned` 결과로 돌려준다. `owned` 없이 받는 낱말(`file.write`, `file.read_line`)에는 몇 번이든 #strong[빌려줄]
수 있다.

두 가지 쓰임을 표로 견주면 이렇다.

#table(columns: (auto, 1fr, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([], [*빌려주기*], [*넘겨주기*]),
  [받는 쪽의 서명], [`(f file …)`: `owned` 가 없다], [`(f owned file …)`, 또는 `owned` 결과],
  [몇 번], [몇 번이든], [모든 경로에서 정확히 한 번],
  [넘긴 뒤], [그대로 쓸 수 있다], [더는 쓰지 못한다],
  [예], [`f file.write`, `f file.read_line`], [`f file.close`, 전역에 넣기, 돌려주기],
)

#ex("manual", "09-copy-file.pnx")

`copy_lines` 에는 파일이 둘이고, 떠나는 길이 넷이다. 둘째 파일을 못 열면 첫째를 닫고 실패한다. 쓰다 실패하면 둘 다 닫고
실패한다. 처리기는 이 모든 경로를 적재 때 따라가서, 어느 하나라도 닫지 않으면 거절한다.

#ex("manual", "09-forgot-close.pnx")

이 낱말은 `read_line` 이 실패하면 `or_fail` 로 떠나는데, 그 길에서 `f` 를 닫지 않는다. 성공하는 길에서도 닫지 않는다.
처리기는 `f` 를 만든 자리를 가리킨다.

#ex("manual", "09-close-twice.pnx")

#qa[결함이 나면 열린 파일은 어떻게 되는가?][결함으로 끝난 실행이 연 핸들은 처리기가 닫는다. 그래서 `owned` 검사는
결함이 아닌 경로들만 본다.]

#mistake[`dup` 으로 하나 더 만들어 두면 편하다][`owned` 값은 `dup` 으로 늘리거나 `drop` 으로 버리지 못한다. 늘리면 두
번 닫게 되고, 버리면 닫지 못한다. 지역 변수에 넣고 이름으로 빌려준다.
#ex("manual", "09-drop-owned.pnx")]

== 구역을 아껴 쓰기

`cat` 은 새 글을 구역에 쓴다. 구역의 크기는 정해져 있으므로, 글을 계속 만드는 루프는 바퀴마다 `mark` 와 `release` 로
구역을 되돌린다. 실패해서 떠나는 길에서도 되돌린다 --- `mark` 가 준 것도 한 번만 쓰는 값이기 때문이다.

#ex("manual", "09-region-loop.pnx")

버퍼에 붙인 바이트는 버퍼에 복사되므로 구역을 되돌려도 남는다.

== 미뤄 두기: `defer`

`copy_lines` 는 실패하는 자리마다 그때까지 연 것을 모두 닫는 코드를 되풀이했다. `defer 코드 end` 는 그 코드를 한 번만
적게 한다. 적어 둔 코드는 그 블록(낱말의 몸, 루프의 한 바퀴, `do` 블록)을 떠나는 모든 길에서 돈다. 끝까지 가도, `or_fail`
로 떠나도, `return` 이나 `break` 로 떠나도 돈다. 여럿이면 적은 차례의 거꾸로 돈다.

#ex("manual", "09-copy-defer.pnx")

`defer src file.close end` 뒤로 `src` 는 `defer` 가 맡는다. 몸에서는 `src file.read_line` 처럼 빌려 쓰기만 하고, 닫는 일은
떠날 때 `defer` 가 한다. 그래서 `or_else` 의 닫는 코드가 사라지고 `or_fail` 을 그대로 쓸 수 있다. 구역의 표시도 같다.

#ex("manual", "09-region-defer.pnx")

#mistake[`defer` 를 적고도 마지막에 직접 닫는다][`defer` 가 맡은 값을 몸에서 넘겨주면 거절된다. 그대로 두면 떠날 때 두 번
닫히기 때문이다. 닫는 일은 `defer` 에 맡긴다.
#ex("manual", "09-defer-close.pnx")]

`defer` 는 낱말의 몸, 루프의 몸, `do` 블록에 바로 쓴다. `if` 의 가지 안에는 쓰지 못한다. 미룬 코드는 스택을 그대로 두어야
하고, 그 안에서 실패하는 낱말은 `or_else` 로 다룬다(표준 10.7).

#summary[
- `owned` 값은 모든 경로에서 정확히 한 번 넘겨준다. 빌려주기는 몇 번이든 된다(표준 12.4).
- 실패의 경로(`or_fail` `or_return`)와 `return` 에서도 넘겨줘야 한다(11.2).
- `defer 코드 end` 는 블록을 떠나는 모든 길에서 돈다. 맡은 값은 몸에서 빌려 쓰기만 한다(10.7).
- `owned` 값은 `dup` `drop` 하지 못한다(12.4).
- 결함이면 처리기가 핸들을 닫는다(11.3).
- 루프의 글은 `mark`/`release` 로 구역을 되돌린다(12.2).
]

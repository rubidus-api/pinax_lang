#import "/tools/common.typ": *

= 실패와 결함 <errors>

#para("1")[일이 잘못되는 길은 둘이다. *실패*는 일어날 수 있다고 본 일이다: 수로 읽히지 않는 글, 가득 찬
목록, 없는 키. 부른 쪽이 그 자리에서 다룬다. *결함*은 프로그램이 틀렸다는 뜻이다: 넘침, 0 으로 나눔,
범위 밖의 번호. 실행을 끝내고 호스트에게 알린다. 스크립트 안에서 결함을 잡는 길은 없다.]

== 오류 번호 <errors-codes>

#para("1")[실패와 결함에는 #term("오류 번호", en: "error code")가 있다. 타입은 `err` 이다. 이름은 `err.` 에
이어 적는 상수다(`use` 없이 쓴다). 낱말 `err_text` 는 번호의 이름을 `str` 로 준다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*상수*], [*이름*], [*뜻*],
  [`err.failed`], [`failed`], [일반적인 실패, `assert` 가 거짓],
  [`err.overflow`], [`overflow`], [정수가 넘침],
  [`err.div_zero`], [`div_zero`], [0 으로 나눔],
  [`err.range`], [`range`], [번호가 범위 밖, 빈 큐·목록에서 꺼냄],
  [`err.full`], [`full`], [용량이 다 참],
  [`err.depth`], [`depth`], [호출 깊이의 한계],
  [`err.stale`], [`stale`], [돌려준 줄이나 핸들을 씀],
  [`err.cancelled`], [`cancelled`], [작업이 취소됨(14절)],
  [`err.interrupted`], [`interrupted`], [호스트가 기다리던 일을 끊음(19절)],
  [`err.deadlock`], [`deadlock`], [남은 작업이 모두 아무것도 바꾸지 못할 큐를 기다림(14.3)],
  [`err.format`], [`format`], [글이나 바이트의 모양이 틀림],
  [`err.not_found`], [`not_found`], [찾는 것이 없음],
)
값은 호스트 인터페이스의 `pinax_err` 와 같다(19.1). 호스트 모듈은 자기 번호를 더할 수 있다.]

#ex("spec", "11-errors.pnx")

== 실패 <errors-failure>

#part("구문")
```ebnf
guard = "or_fail" | "or_return" | "or_else" [ "(" NAME ")" ] { body } "end" ;
```

#para("1")[서명이 `or fail` 로 끝나는 낱말은 실패할 수 있다. 실패하면 결과는 없고 오류 번호만 있다.]

#para("2")[실패할 수 있는 낱말을 부른 바로 다음 토큰은 #term("지킴 낱말", en: "guard")이어야 한다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*지킴*], [*실패했을 때*],
  [`or_fail`], [같은 오류로 이 낱말도 실패한다. 이 낱말의 서명에 `or fail` 이 있어야 한다],
  [`or_return`], [이 낱말을 끝낸다. 그 자리의 스택이 결과와 맞아야 한다],
  [`or_else … end`], [몸이 대신할 결과를 만든다. `or_else (e) … end` 는 오류 번호를 지역 변수 `e` 로 받는다. 몸은 `return` `fail` `break` 로 떠나도 된다],
)
성공하면 지킴 낱말은 아무 일도 하지 않고 결과가 스택에 남는다.]

#para("2a")[`str(N)` 자리에 넣는 일 뒤에도 지킴 낱말이 올 수 있다. 그러면 글이 너무 길 때 `range` 로 실패한다
(7.3 (4a)). 다른 넣기 뒤의 지킴 낱말은 거절된다.]

#para("3")[`번호 fail` 은 이 낱말을 그 오류로 실패시킨다. 이 낱말의 서명에 `or fail` 이 있어야 한다.]

#ex("spec", "11-failure.pnx")
#ex("spec", "11-guards.pnx")

#para("4")[넘침과 0 으로 나눔을 결함 대신 실패로 받으려면 `chk_add` `chk_sub` `chk_mul` `chk_div` 를 쓴다
(15.1).]

#ex("spec", "11-chk.pnx")

#part("제약")

#para("5")[실패할 수 있는 낱말 뒤에 지킴 낱말이 없으면 거절된다. 실패를 모르고 지나칠 수 없다.]
#ex("spec", "11-guard-missing.pnx")

#para("6")[`or_fail` 과 `fail` 은 서명에 `or fail` 이 있는 낱말 안에서만 쓴다. 정의 밖의 코드도 `or fail` 이
없는 것으로 친다.]
#ex("spec", "11-or-fail-needs.pnx")
#ex("spec", "11-fail-needs.pnx")

#para("7")[`or_else` 의 몸은 낱말이 성공했다면 남겼을 값을 남기거나, 떠나야 한다.]
#ex("spec", "11-or-else-stack.pnx")

#para("8")[실패의 경로에서도 `owned` 값은 넘겨줘야 한다(12.4). 처리기는 `or_fail` `or_return` 자리마다
검사한다.]

== 결함 <errors-fault>

#para("1")[결함은 그 자리에서 실행을 끝낸다. 결함은 호스트가 부른 함수(`pinax_run`, `pinax_load` 등)의
반환값이 된다. 작업 안의 결함도 그 실행 전체와 모든 작업을 끝낸다. 작업의 묶음이 `apart` 로 선언되었으면 그렇지 않다(14.4).
상태는 남아 다음 실행을 받는다.]

#para("2")[결함은 다음에서 난다.
- 산술의 넘침(`overflow`), 0 으로 나눔(`div_zero`) --- 7.1, 15.1;
- 배열·목록의 범위 밖 번호, 맞는 가지가 없는 `case`, 없음 줄, 지킴 낱말 없이 넣은 `str(N)` 보다 긴 글(`range`)
  --- 7.3, 8, 10.3;
- 좁은 필드에 맞지 않는 값(`overflow`) --- 8.8;
- 구역이나 작업 공간이 모자람(`full`) --- 12.2;
- 호출 깊이(`depth`) --- 9.6;
- 돌려준 줄이나 핸들(`stale`) --- 8.9, 19.6;
- 거짓인 `assert`(`failed`).]

#para("3")[결함으로 끝난 실행이 구역에 만든 것은 되감긴다. 실행이 연 핸들은 처리기가 닫는다. 그래서 결함의
경로에서 `owned` 값을 손으로 닫을 일은 없다. 전역에 이미 넣은 값과 호스트 낱말이 이미 한 일은 그대로 남는다.]

#para("4")[적합한 처리기는 결함의 갈래와 그것이 난 줄을 알린다. `pnx` 는 그 아래에 결함이 난 낱말의 지역
변수를 함께 보인다.]

#ex("spec", "11-div-zero.pnx")
#ex("spec", "11-assert.pnx")

#part("진단")
#diag(
  ([실패할 수 있는 낱말 뒤에 지킴이 없음], "a word that can fail is followed by or_fail, or_return or or_else"),
  ([`or fail` 없는 낱말의 `or_fail`], "or_fail passes the failure on, so this word needs `or fail` in its signature"),
  ([`or fail` 없는 낱말의 `fail`], "fail needs `or fail` in the signature of this word"),
  ([`or_else` 의 몸이 남기는 값이 다름], "the body of or_else must leave what the word would have given, or leave the word or the loop"),
)

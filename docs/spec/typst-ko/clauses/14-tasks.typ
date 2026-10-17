#import "/tools/common.typ": *

= 작업 <tasks>

#part("구문")
```ebnf
tasks-decl = "tasks" NAME size [ size [ size ] ] ;   (* count, call depth, region bytes *)
```

#para("1")[*작업*은 한 상태 안에서 번갈아 도는 실행 흐름이다. 작업은 스스로 양보하는 자리(`yield`, 기다리는
호스트 낱말)에서만 바뀐다. 그래서 두 작업이 같은 전역을 쓰더라도, 한 작업의 두 양보 사이에서는 다른 작업이 끼어들지
않는다. 작업은 스레드가 아니다 --- 한 상태는 한 번에 한 스레드에서만 돈다(5.1).]

#para("2")[`tasks 이름 수 [깊이 [구역]]` 은 #term("작업 묶음", en: "task group")을 선언한다. 묶음은 작업을 `수`
개까지 담는다. 작업마다 자기 호출 깊이(`깊이`)와 자기 구역(`구역` 바이트)이 있다. 줄이면 처리기의 기본값이다.
묶음의 이름은 그 묶음의 작업을 가리키는 값의 타입이다.]

== 만들기와 돌리기 <tasks-run>

#para("1")[`블록 묶음 spawn` 은 받는 것도 주는 것도 없는 값이 되는 블록(10.5)으로 작업 하나를 만들고 그 작업을
준다(`or fail`, 빈 자리가 없으면 `err.full`). 작업은 만들어진 채 멈춰 있고, 아직 돌지 않는다.]

#para("2")[`묶음 run_all` 은 그 묶음의 작업을 차례로, 모두 끝날 때까지 돌린다. 한 작업은 양보하거나 끝날 때까지
돌고, 그다음 작업이 이어 돈다.]

#ex("spec", "14-tasks.pnx")

#para("3")[`작업 resume` 은 그 작업 하나를 다음 양보나 끝까지 돌리고, 끝났으면 `true` 를 준다. `작업 is_done`
은 끝났는지 묻는다. 끝난 작업의 자리는 다시 `spawn` 에 쓰인다.]

#ex("spec", "14-resume.pnx")

#para("4")[작업은 상태 안에 남는다. 호스트가 부른 한 실행이 끝나도 멈춘 작업은 그대로 있고, 다음 실행에서 이어
돌릴 수 있다.]

#ex("spec", "14-full.pnx")

== 양보 <tasks-yield>

#para("1")[`yield` 는 다른 작업에 차례를 넘긴다. 실패할 수 있다(`or fail`): 작업이 취소되었으면
`err.cancelled` 로 실패한다. 작업이 아닌 주 실행에서 `yield` 는 아무것도 하지 않고 바로 성공한다.]

#para("2")[작업 사이의 자료는 전역, 큐, 목록으로 나눈다. 가득 찬 큐나 빈 큐는 양보할 까닭이 된다.]

#ex("spec", "14-queue.pnx")

#para("3")[`yield` 를 (직접이든, 부른 낱말 안에서든) 할 수 있는 낱말은 적재 때 알려진다. `wait_take` 와
`wait_put`(14.3)도 양보한다. 호스트 낱말 안에서 그런 낱말은 돌리지 못한다(19절).]

== 큐 기다리기 <tasks-queue-wait>

#para("1")[`큐 wait_take` 는 `take` 처럼 큐의 맨 앞 값을 꺼내되, 큐가 비어 있는 동안은 양보하고 차례가 돌아오면
다시 본다. `v 큐 wait_put` 은 `put` 처럼 넣되, 큐가 차 있는 동안 양보한다. 둘 다 실패할 수 있다(`or fail`):
기다리는 동안 취소된 작업은 `err.cancelled` 로 실패한다. 값을 바로 꺼내거나 넣을 수 있을 때는 취소를 보지 않는다.]

#ex("spec", "14-queue-wait.pnx")

#para("2")[큐를 기다리는 작업은 어떤 큐든 바뀌기 전에는 다시 돌지 않는다. `run_all` 이 남은 작업이 모두 큐를
기다리고 호스트를 기다리는 작업은 없다는 것을 알면, 이제 큐를 바꿀 것이 없다: 이것이 결함 `deadlock` 이다. 작업
안의 결함처럼 모든 작업을 끝낸다. 호스트를 기다리는 작업이 있으면 상태는 전처럼 기다리고(`PINAX_WAITING`), 그
작업이 이어 돌며 하는 일이 다른 작업을 풀 수 있다.]

#ex("spec", "14-deadlock.pnx")

#para("3")[주 실행은 작업이 아니어서 차례를 넘길 데가 없다. 주 실행에서 빈 큐의 `wait_take`, 찬 큐의 `wait_put` 은
바로 결함 `deadlock` 이다.]

#ex("spec", "14-deadlock-main.pnx")

#para("4")[교착을 보는 것은 이 두 낱말뿐이다. 스스로 도는 대기(`loop … yield … end`)나 `resume` 으로 손수 돌리는
작업은 보지 않는다. 그런 코드를 돌리는 호스트는 예산을 준다(19.5).]

== 취소 <tasks-cancel>

#para("1")[`작업 cancel` 은 작업에 취소를 알린다. 그 작업의 다음 `yield` 나 큐 기다림(14.3)이 `err.cancelled` 로
실패한다. 작업은
그 실패를 받아 자기 방식으로 끝난다 --- 취소는 작업을 억지로 멈추지 않는다.]

#ex("spec", "14-cancel.pnx")

#part("제약")

#para("2")[`spawn` 에 주는 블록은 값이 되는 블록이라 바깥의 지역 변수를 보지 못한다. 작업에 값을 주려면 전역이나
큐를 쓴다.]
#ex("spec", "14-spawn-refused.pnx")

#para("2a")[`spawn` 의 블록은 받는 것도 주는 것도 없어야 한다. 매개변수가 있는 블록은 거절된다.]
#ex("spec", "14-spawn-block.pnx")

#part("결함")

#para("3")[작업 안의 결함은 그 실행 전체와 모든 작업을 끝낸다. 결함은 주 실행의 결함처럼 호스트에게 간다.
상태는 남아 다음 실행을 받는다.]
#ex("spec", "14-fault.pnx")

#para("4")[두 작업이 같은 구역을 쓰는 일은 12.3 의 검사가 보지 않는다. 작업마다 자기 구역이 있으므로 보통은
일어나지 않는다.]

#part("진단")
#diag(
  ([매개변수나 결과가 있는 블록을 `spawn` 에 줌], "spawn takes a block that takes and gives nothing, and a set of tasks: do ... end workers spawn"),
  ([작업이 아닌 값에 `resume` `is_done` `cancel`], "this word takes a task"),
  ([작업 묶음이 아닌 모음(배열, 목록)에 `run_all`], "run_all takes a set of tasks"),
  ([`tasks` 의 수가 4096 을 넘음], "a set holds at most 4096 tasks"),
  ([작업의 호출 깊이가 1 부터 4096 밖], "the call depth of a task is 1 to 4096"),
  ([작업의 구역이 16 MiB 를 넘음], "the region of a task is at most 16 MiB"),
  ([자료 소스의 `tasks`], "a data source runs no code and declares no tasks"),
)

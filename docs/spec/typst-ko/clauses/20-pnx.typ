#import "/tools/common.typ": *
// shape: the command; its refusals are those of the language

= 명령 `pnx` <pnx>

#para("1")[`pnx` 는 이 라이브러리를 쓰는 호스트이자 개발 도구다. 표준 호스트 모듈(17절)을 모두 등록한다.
```text
pnx [--lib NAME=PATH]... file.pnx [arguments]
pnx test file.pnx
pnx repl
pnx see file.pnx
pnx fmt file.pnx
pnx export-c file.pnx [NAME]
```]

== 돌리기 <pnx-run>

#para("1")[`pnx file.pnx 인자…` 는 파일을 적재하고 적재 코드를 돌린다. 파일이 `use` 하는 모듈 가운데 등록되지
않은 것은 파일 옆의 `모듈.pnx` 에서 먼저 적재한다(13.1). 파일은 이름에서 `.pnx` 를 뗀 모듈로 적재된다 --- 그
이름이 낱말처럼 적히지 않으면(숫자로 시작하거나 `-` 를 담으면) 이름 없는 모듈이 된다.]

#para("2")[종료 상태는 셋이다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*상태*], [*언제*],
  [0], [적재 코드가 끝까지 돌았다. `pnx test` 는 모든 시험이 통과했다],
  [1], [거절: `파일:줄:칸: 까닭` 을 표준 오류에. 결함: `파일:줄: 갈래` 와 그 낱말의 지역 변수를. `pnx test` 는 실패한 시험이 있다],
  [2], [명령이 틀렸다(사용법을 보인다), 파일을 읽지 못한다, `--lib` 의 라이브러리를 열지 못한다],
)]

#para("3")[`--lib NAME=PATH` 는 `PATH` 의 공유 라이브러리를 열어 `extern … from "NAME"`(18절)의 해결 함수가 그
라이브러리의 함수를 찾게 한다. 그 라이브러리가 정의한 함수만 찾는다(그것이 들여온 함수는 찾지 않는다). `--lib` 가
없으면 `extern` 은 모두 거절된다.]

== 시험 <pnx-test>

#part("구문")
```ebnf
test = "test" NAME { body } "end" ;
```

#para("1")[`test 이름 … end` 는 #term("시험", en: "test")을 정의한다. 시험은 받는 것도 주는 것도 없는 낱말이다.
적재할 때는 돌지 않는다. `pnx test file.pnx` 는 그 파일의 시험을 차례로 돌리고, 시험마다 `ok` 나 `FAIL` 을 쓰고,
모두 통과하면 0, 하나라도 실패하면 1 로 끝난다. 시험 안의 결함(거짓인 `assert` 를 포함해)은 그 시험의 실패다.]

#ex("spec", "20-test.pnx")
#ex("spec", "20-test-fail.pnx")

== 대화 <pnx-repl>

#para("1")[`pnx repl` 은 표준 입력에서 한 줄씩 읽어 돌린다(19.4 의 `pinax_eval`). 줄이 남긴 값을 표준 출력에 쓴다.
앞 줄에서 정의한 것을 뒤 줄에서 쓸 수 있다. 정의나 블록이 끝나지 않은 줄은 다음 줄과 이어 읽는다. 거절되거나
결함으로 끝난 줄은 아무것도 남기지 않는다.]

#para("2")[대화에서만 쓰는 도구 낱말이 있다: `list_words`(정의한 낱말), `stack`(그 자리의 스택의 타입),
`see`(뒤따르는 정의의 줄마다 스택), `redefine word …`(같은 서명으로 낱말을 바꿈. 그 낱말을 부르는 코드도 새것을
부른다).]

#ex("spec", "20-repl.pnx")

== 스택 보기와 정리 <pnx-tools>

#para("1")[`pnx see file.pnx` 는 정의의 줄마다 그 줄이 끝난 자리의 스택(타입)을 보인다.]

#ex("spec", "20-see.pnx")

#para("1a")[소스가 거절되면 `pnx see` 는 거절된 자리 앞의 줄들을 보이고, 거절 진단을 내고, 1 로 끝난다.]

#ex("spec", "20-see-refused.pnx")

#para("1b")[`pnx see --checks file.pnx` 는 파일을 `pnx file.pnx` 처럼 적재하고(정의 밖의 코드가 돈다), 그 다음 낱말마다
적재기가 «울릴 수 없다»고 증명한 실행 때의 검사를 보인다: 낱말의 검사 수와 증명된 수, 그리고 증명된 명령마다 그 줄, 명령,
까닭. 증명은 프로그램의 동작을 바꾸지 않는다. 검사는 그대로 돈다.]

#ex("spec", "20-see-checks.pnx")

#para("2")[`pnx fmt file.pnx` 는 들여쓰기와 공백만 고친 글을 표준 출력에 쓴다. 토큰과 줄바꿈의 자리, 주석은
그대로다. 고친 글을 다시 고쳐도 같다.]

#ex("spec", "20-fmt.pnx")

#para("3")[`pnx export-c file.pnx [NAME]` 은 `pnx file.pnx` 처럼 파일을 적재하되 그 코드는 하나도 돌리지 않고, 프로그램의
코드를 C 로 표준 출력에 쓴다. `pinax_native_attach`(19.11)에 줄 표 `NAME`(빠지면 `pnx_native`)을 정의하는 파일이다.]

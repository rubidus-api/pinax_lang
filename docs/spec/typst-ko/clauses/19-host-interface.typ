#import "/tools/common.typ": *
// shape: the C interface; its examples are programs that run

= 호스트 인터페이스 <host>

#para("1")[호스트 인터페이스는 C 헤더 `pinax.h` 하나다. 그 헤더는 `<stddef.h>` 와 `<stdint.h>` 만 쓰고 C99, C23,
C++ 컴파일러로 읽힌다. 이 절은 그 함수들이 무엇을 하는지 정한다. 함수마다 헤더의 주석이 더 자세하다.]

#para("2")[한 상태는 한 번에 한 스레드만 쓴다. 상태 사이에는 나누는 것이 없다. 함수가 돌려주는 `pinax_str` 는
따로 적지 않으면 그 상태에 대한 다음 호출 전까지만 쓸 수 있다.]

== 오류 번호 <host-errors>

#para("1")[함수는 `pinax_err` 를 돌려준다. `PINAX_OK` 는 0 이다. 실패가 아닌 결과 셋이 있다:
`PINAX_SUSPENDED`(예산이 다함, 19.5), `PINAX_WAITING`(모든 작업이 호스트를 기다림, 19.8), `PINAX_PENDING`(호스트
낱말이 나중에 끝남). `0x100` 부터는 스크립트의 결함과 실패(11.1 의 번호와 같다), `0x200` 부터는 인터페이스 자체의
실패다: `ARGUMENT`, `NO_ROOM`(덩이가 모자람), `SYNTAX`, `TYPE`(적재 거절), `NOT_FOUND`, `SIGNATURE`, `FORMAT`,
`STATE`(지금 할 수 없는 호출).]

#para("2")[`pinax_error_text` 는 마지막 실패를 글로 준다. 결함이면 `모듈:줄: 이름` 꼴이다.]

== 열기와 닫기 <host-open>

#para("1")[`pinax_open(block, config, &state)` 은 덩이 `block` 안에 상태를 연다. `config` 의 0 인 칸은 처리기의
기본값이다: 뱅크의 칸 수, 주 실행의 구역 크기, 동시에 살 수 있는 핸들의 수, 실행 한 번의 명령 예산(0 이면 끝이
없다), 적재 진단을 받을 함수. 덩이가 모자라면 `PINAX_ERR_NO_ROOM` 이고, `pinax_needed` 가 넉넉했을 크기를
알려 준다.]

#para("2")[`config.hash_key` 는 찾아보기(8.10)가 쓰는 해시의 열쇠 16 바이트다. 밖에서 온 글을 키로 찾는 호스트는
무작위 열쇠를 준다. 모두 0 이면 정해진 열쇠를 쓴다.]

#para("3")[`pinax_close` 는 아직 열린 핸들을 닫는다(그 타입의 닫는 함수를 부른다). 덩이는 놓지 않는다 --- 덩이는
호스트의 것이다.]

== 호스트 낱말 <host-words>

#para("1")[`pinax_register(state, 모듈, defs, count)` 는 호스트 함수를 모듈 하나의 낱말로 등록한다. `defs` 의 칸마다
이름, 서명 글(9.2 의 꼴, 예: `"(s str gives n int or fail)"`), 함수, 사용자 포인터, 깃발이 있다. 서명은 등록할 때
한 번 검사한다. 그 낱말을 쓰는 소스보다 먼저 등록한다.]

#para("2")[호스트 함수는 `(call, user, args)` 를 받는다. 인자와 결과는 타입별 배열에 있다: 정수(그리고 `bool`, 문자,
핸들, `int` 위의 선언한 타입)는 `args->i[]` 와 `args->ri[]`, 문자열은 `args->s[]` 와 `args->rs[]`. 서명에 적힌
차례로, 타입마다 0 부터 센다. 정수 포인터는 상태의 칸을 바로 가리킨다.]

#para("3")[함수가 `PINAX_OK` 가 아닌 값을 돌려주면, 서명에 `or fail` 이 있는 낱말은 그 번호로 실패하고(11.2) 없는
낱말은 결함으로 실행을 끝낸다.]

#para("4")[문자열 결과는 `PINAX_WORD_KEEP_STR` 깃발이 없으면 부른 쪽의 구역으로 복사된다. 새 바이트를 만들 자리는
`pinax_call_alloc` 이 부른 쪽의 구역에서 준다. 서명에서 `buffer` 타입으로 받은 매개변수는 스크립트의 버퍼다.
`pinax_call_buffer` 로 그 버퍼의 빈 자리를 얻고, 채운 바이트 수를 알린다.]

#ex("spec", "19-host-words.c")

== 적재와 한 줄 <host-load>

#para("1")[`pinax_load(state, 이름, 소스, 방식)` 은 소스를 적재한다(5.2). `이름` 은 모듈의 이름이다. 방식은
`PINAX_LOAD_CODE` 와 `PINAX_LOAD_DATA`(13.4)다. 거절하면 `PINAX_ERR_SYNTAX` 나 `PINAX_ERR_TYPE` 이고, 진단은
`config.diag` 에 (파일, 줄, 칸, 글)로 온다.]

#para("2")[`pinax_eval(state, 글)` 은 대화처럼 한 줄을 돌린다. 모든 줄은 한 모듈(세션)이다. 줄이 남긴 값은
`pinax_eval_count` 와 `pinax_eval_value` 로 읽는다.]

== 찾기와 돌리기 <host-run>

#para("1")[`pinax_find(state, 이름, 서명, 깃발, &word)` 은 낱말을 찾고 그 서명이 주어진 서명과 같은지 본다. 다르면
`PINAX_ERR_SIGNATURE` 다. 맨 이름은 처음 적재한 소스의 낱말이고, `모듈.낱말` 은 그 모듈의 낱말이다(내보내지
않았어도).]

#para("2")[`pinax_run(state, word, args)` 은 낱말을 돌린다. 예산이 다하면 `PINAX_SUSPENDED` 로 돌아오고,
`pinax_resume` 이 멈춘 자리부터 잇는다. 예산은 결과를 바꾸지 않는다. 결함은 그 번호로 돌아오고, 상태는 다음 실행을
받는다.]

#para("3")[멈춘 실행(`PINAX_SUSPENDED`, `PINAX_WAITING`)이 있는 동안 새 `pinax_run` 은 `PINAX_ERR_STATE` 다. 호스트는
그 실행을 끝까지 잇거나 `pinax_abort` 로 버린다.]

#para("3a")[`pinax_abort(state)` 는 멈춘 실행을 결함으로 끝난 것처럼 끝낸다: 그 실행이 연 핸들을 닫고, 작업을 비우고,
기다리던 표를 잊는다(그 표의 `pinax_complete` 는 `PINAX_ERR_NOT_FOUND`). 전역에 이미 넣은 값은 남는다. 멈춘 것이
적재 코드면 그 적재를 실패한 적재처럼 되돌린다. 그 뒤 상태는 다음 실행을 받는다. 멈춘 실행이 없으면
`PINAX_ERR_STATE` 다. 전역까지 실행 앞으로 되돌려야 하는 호스트는 실행 앞에 상(19.9)을 저장한다.]

#ex("spec", "19-run.c")

#para("4")[호스트 낱말 안에서도 `pinax_run` 을 부를 수 있다. 그때는 `PINAX_FIND_NESTED` 깃발로 찾은 낱말만 돌리며,
양보할 수 있는 낱말(14.2)은 그 깃발로 찾지 못한다.]

== 핸들 <host-handles>

#para("1")[`pinax_handle_type` 은 핸들 타입 하나를 이름과 닫는 함수로 만든다. 스크립트는 그 이름을 서명의 타입으로
쓴다. `pinax_handle_new` 는 C 포인터를 핸들로 만들고, `pinax_handle_get` 은 핸들의 포인터를 준다(돌려준 핸들이면
NULL). `pinax_handle_release` 는 핸들을 돌려준다.]

#para("2")[핸들은 번호와 세대다(8.9 와 같다). 스크립트는 핸들로 주소를 얻지 못한다. 결함으로 끝난 실행이 연 핸들은
상태가 닫는 함수를 불러 닫는다.]

#ex("spec", "19-handles.c")

== 묶기 <host-bind>

#para("1")[`pinax_bind(state, 전역, 메모리, 바이트)` 는 `bound` 로 선언한 전역(8.12)의 저장소를 호스트 메모리로
바꾼다. 크기는 선언과 같아야 한다. 스크립트와 호스트는 복사 없이 같은 바이트를 읽고 쓴다. `pinax_global_find` 는
선언한 전역의 자리와 크기를 알려 준다.]

#ex("spec", "19-bind.c")

== 나중에 끝나는 호스트 낱말 <host-await>

#para("1")[`PINAX_WORD_AWAITABLE` 로 등록한 호스트 낱말은 일을 시작하고 `pinax_call_pending` 으로 표를 받은 뒤
`PINAX_PENDING` 을 돌려줄 수 있다. 부른 작업은 기다리고 다른 작업이 돈다. 돌 것이 없으면 `pinax_run` 이나
`pinax_resume` 이 `PINAX_WAITING` 을 돌려준다. 호스트는 `pinax_complete(state, 표, 결과, 값)` 로 일을 끝내고
`pinax_resume` 을 부른다.]

#ex("spec", "19-await.c")

== 상과 복제 <host-image>

#para("1")[#term("상", en: "image")은 상태 전체다: 코드, 선언, 전역, 칸, 구역, 멈춘 실행과 작업. `pinax_image_save`
가 쓰고 `pinax_image_open` 이 연다. 상이 덩이의 처음에 놓여 있으면 복사 없이 그 자리에서 연다. 다른 형식이나 다른
빌드의 메모리 배치로 만든 상은 `PINAX_ERR_FORMAT` 이다.]

#para("2")[연 상태에는 호스트 낱말, 핸들 타입, `extern` 함수를 이름으로 다시 준다. 다 주기 전에는 실행이
`PINAX_ERR_NOT_FOUND` 로 거절된다. 핸들은 닫힌 채(닫는 함수 없이), 묶은 전역은 자기 저장소로 돌아와 있다.
`pinax_clone` 은 한 번에 저장하고 다른 덩이에서 연다.]

#ex("spec", "19-image.c")

== 재기와 결함의 지역 변수 <host-measure>

#para("1")[`pinax_stats_get` 은 칸, 구역, 핸들, 호출 깊이의 가장 높았던 값과 돈 명령의 수를 준다. 덩이와 용량을
정할 때 쓴다.]

#para("2")[`pinax_fault_local` 은 결함으로 끝난 실행에서 결함이 난 낱말의 지역 변수를 그때의 값으로 준다(16 개까지,
글은 처음 32 바이트). `pnx` 가 결함 아래에 보이는 것이 이것이다(11.3).]

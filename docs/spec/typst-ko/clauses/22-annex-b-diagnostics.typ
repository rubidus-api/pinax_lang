#import "/tools/common.typ": *
// shape: collected from the clauses

#set heading(numbering: (..n) => if n.pos().len() == 1 { "부속서 B" } else { "B." + n.pos().slice(1).map(str).join(".") })
= 진단 목록 (참고) <annex-diagnostics>

#para("1")[이 부속서는 본문의 진단 표에 나온 메시지를 모은다. 메시지는 이 구현이 내는 글이다(4 (3)). 왼쪽은 그
메시지를 적은 조항이다.]

#para("2")[거절 메시지는 `파일:줄:칸: 메시지` 로 찍힌다. 줄과 칸은 처리기가 거절을 안 토큰의 자리다 --- 빠진 것은 그 뒤의
토큰에서, 남은 값은 정의의 끝에서 알려진다. 메시지 안의 `name`, `s`, `i`, `buf` 는 자리의 이름이지 낱말이 아니고, 콜론 뒤의
짧은 코드는 맞는 꼴의 보기다.]

#all-diagnostics()

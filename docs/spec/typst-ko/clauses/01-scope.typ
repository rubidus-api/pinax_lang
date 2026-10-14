#import "/tools/common.typ": *
= 범위 <scope>
#para("1")[이 문서는 #term("처리기", en: "processor") 를 정한다.]
#part("제약")
#para("2")[문자열을 받는 낱말에 정수를 줄 수 없다.]
#ex("spec", "hello.pnx")
#ex("spec", "hello-refused.pnx")
#qa[무엇인가?][이것이다.]
#diag(([타입이 다르다], "an argument has another type"),)

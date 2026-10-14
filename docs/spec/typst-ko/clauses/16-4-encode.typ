#import "/tools/common.typ": *
// shape: failures only; no load-time constraint of its own

== `encode` --- 바이트를 글로 <std-encode>

#para("1")[모듈 `encode` 의 낱말은 모두 `(s str, buf or fail)` 이다. 바꾼 결과를 버퍼에 붙이고, 자리가
모자라면 `err.full`, 입력의 모양이 틀리면 `err.format` 으로 실패한다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*뜻*],
  [`base64_encode` `base64_decode`], [RFC 4648 의 Base64. 풀 때 끝의 `=` 는 없어도 된다],
  [`hex_encode` `hex_decode`], [바이트마다 십육진 두 자리. 쓸 때 소문자, 읽을 때 대소문자 모두],
  [`url_encode` `url_decode`], [RFC 3986 의 퍼센트 부호화. 문자, 숫자, `-` `.` `_` `~` 밖의 바이트를 `%XX` 로],
)]

#ex("spec", "16-encode.pnx")
#ex("spec", "16-encode-bad.pnx")

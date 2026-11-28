# Pinax 매뉴얼 · The Pinax Manual

Pinax 를 처음부터 끝까지 배우고 쓰는 책이다. 장마다 그 장이 기대는 것과 답하는 물음으로 시작하고, 돌아가는 예제와
거절되는 예제, 흔한 오해를 싣고, 표준의 조항을 붙인 정리로 끝난다. 한국어판이 원본이고 영어판은 같은 장과 같은 예제를
쓴다. 이 책과 표준이 어긋나면 표준이 맞다.

A book that teaches Pinax from the first program to embedding it in C. Each chapter opens with what it builds on and
the questions it answers, shows running and refused programs and common mistakes, and ends with a summary that points
to the clauses of the standard. Korean original, English edition with the same chapters and examples.

| 폴더 · folder | 무엇 · what |
|---|---|
| 웹판 · web | <https://rubidus-api.github.io/pinax_lang/manual/html-ko/> · <https://rubidus-api.github.io/pinax_lang/manual/html-en/> |
| PDF | 릴리스마다 · with each release |
| [`examples/`](examples/) | 문서가 싣는 모든 프로그램 · every program the document shows |

원고(Typst)는 공개하지 않는다. 웹판과 PDF 는 릴리스 때 지어 웹과 릴리스에 올린다. The manuscript (Typst) is not
published; the web editions and the PDFs are built at each release.

## 예제는 돌린 것이다 · Every example is run

`examples/` 의 파일마다 첫 줄에 무엇이 일어나야 하는지 적혀 있다(`rem run`, `rem refused`, `rem fault`, `rem test`,
`// embed` …). [`../tools/verify-examples.sh`](../tools/verify-examples.sh) 가 그것을 확인하고 출력을 받아 두며, 원고는 받아
둔 출력만 싣는다. 거절되는 프로그램의 메시지도 처리기가 낸 그대로다.

Each file in `examples/` says on its first line what must happen. `../tools/verify-examples.sh` checks it and captures
the output; the manuscript prints only what was captured, refusals included.

## 예제 돌리기 · Running the examples

```sh
./nob                                     # pnx 와 라이브러리 · pnx and the library
sh docs/tools/verify-examples.sh manual      # 예제마다 첫 줄의 약속 · each example against its first line
```

## 라이선스 · License

본문은 CC BY-NC-SA 4.0, 예제는 MIT 다. [`LICENSE-NOTICE.md`](LICENSE-NOTICE.md) 를 본다.
Text under CC BY-NC-SA 4.0, examples under MIT; see [`LICENSE-NOTICE.md`](LICENSE-NOTICE.md).

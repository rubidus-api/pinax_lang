# Pinax 매뉴얼 · The Pinax Manual

Pinax 를 처음부터 끝까지 배우고 쓰는 책이다. 장마다 그 장이 기대는 것과 답하는 물음으로 시작하고, 돌아가는 예제와
거절되는 예제, 흔한 오해를 싣고, 표준의 조항을 붙인 정리로 끝난다. 한국어판이 원본이고 영어판은 같은 장과 같은 예제를
쓴다. 이 책과 표준이 어긋나면 표준이 맞다.

A book that teaches Pinax from the first program to embedding it in C. Each chapter opens with what it builds on and
the questions it answers, shows running and refused programs and common mistakes, and ends with a summary that points
to the clauses of the standard. Korean original, English edition with the same chapters and examples.

| 폴더 · folder | 무엇 · what |
|---|---|
| [`typst-ko/`](typst-ko/) · [`typst-en/`](typst-en/) | 원고(Typst) --- 원본 · the manuscript, the source |
| [`md-ko/`](md-ko/README.md) · [`md-en/`](md-en/README.md) | GitHub 에서 읽는 Markdown 판 · Markdown editions |
| [`html-ko/`](html-ko/index.html) · [`html-en/`](html-en/index.html) | 웹 판, 읽기는 <https://rubidus-api.github.io/pinax_lang/manual/html-ko/> · web editions, read at <https://rubidus-api.github.io/pinax_lang/manual/html-en/> |
| `pdf-ko/` · `pdf-en/` | PDF 판(짓기만 하고 커밋하지 않는다) · PDF editions, built, not committed |
| [`examples/`](examples/) | 책이 싣는 모든 프로그램 · every program the book shows |

## 예제는 돌린 것이다 · Every example is run

`examples/` 의 파일마다 첫 줄에 무엇이 일어나야 하는지 적혀 있다(`rem run`, `rem refused`, `rem fault`, `rem test`,
`// embed` …). [`../tools/verify-examples.sh`](../tools/verify-examples.sh) 가 그것을 확인하고 출력을 받아 두며, 원고는 받아
둔 출력만 싣는다. 거절되는 프로그램의 메시지도 처리기가 낸 그대로다.

Each file in `examples/` says on its first line what must happen. `../tools/verify-examples.sh` checks it and captures
the output; the manuscript prints only what was captured, refusals included.

## 짓기 · Building

```sh
./nob                                   # pnx 와 라이브러리 · pnx and the library
sh docs/tools/build.sh manual             # 예제 → PDF → HTML → Markdown → 점검 · examples, PDF, HTML, Markdown, checks
sh docs/tools/build.sh manual quick check # 원고가 조판되는지만 · just check the manuscripts compile
```

[Typst](https://typst.app) 0.15 와 Noto Serif CJK KR, Noto Sans CJK KR, D2Coding 글꼴이 필요하다. `TYPST` 와
`TYPST_FONT_PATHS` 로 알려 준다. 글꼴을 못 찾으면 PDF 를 버린다 --- Typst 는 대체 글꼴로 조용히 성공하기 때문이다.
Needs Typst 0.15 and the fonts above (set `TYPST` and `TYPST_FONT_PATHS`); a PDF set in substituted fonts is discarded.

## 라이선스 · License

본문은 CC BY-NC-SA 4.0, 예제는 MIT 다. [`LICENSE-NOTICE.md`](LICENSE-NOTICE.md) 를 본다.
Text under CC BY-NC-SA 4.0, examples under MIT; see [`LICENSE-NOTICE.md`](LICENSE-NOTICE.md).

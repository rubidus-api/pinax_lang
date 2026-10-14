# Pinax 표준 · The Pinax Standard

끼워 쓰는 스크립트 언어 Pinax 의 정의다. 무엇이 적합한 프로그램인지, 그것이 돌 때 무엇을 하는지, 처리기가 무엇을
거절하는지, C 호스트가 처리기를 어떻게 부르는지를 번호 붙은 조항으로 정한다. 한국어판이 원본이고 영어판은 같은 조항과
같은 문단 번호를 쓴다.

The definition of the embeddable scripting language Pinax, in numbered clauses: what a conforming program is, what it
does when it runs, what a processor refuses, and how a C host drives it. The Korean edition is the original; the English
edition has the same clauses and paragraph numbers.

| 폴더 · folder | 무엇 · what |
|---|---|
| [`typst-ko/`](typst-ko/) · [`typst-en/`](typst-en/) | 원고(Typst) --- 원본 · the manuscript, the source |
| [`md-ko/`](md-ko/README.md) · [`md-en/`](md-en/README.md) | GitHub 에서 읽는 Markdown 판 · Markdown editions |
| [`html-ko/`](html-ko/index.html) · [`html-en/`](html-en/index.html) | 웹 판 · web editions |
| `pdf-ko/` · `pdf-en/` | PDF 판(짓기만 하고 커밋하지 않는다) · PDF editions, built, not committed |
| [`examples/`](examples/) | 표준이 싣는 모든 프로그램 · every program the standard shows |

## 예제는 돌린 것이다 · Every example is run

`examples/` 의 파일마다 첫 줄에 무엇이 일어나야 하는지 적혀 있다(`rem run`, `rem refused`, `rem fault`, `rem test`,
`// embed` …). [`../tools/verify-examples.sh`](../tools/verify-examples.sh) 가 그것을 확인하고 출력을 받아 두며, 원고는 받아
둔 출력만 싣는다. 거절되는 프로그램의 메시지도 처리기가 낸 그대로다.

Each file in `examples/` says on its first line what must happen. `../tools/verify-examples.sh` checks it and captures
the output; the manuscript prints only what was captured, refusals included.

## 짓기 · Building

```sh
./nob                                   # pnx 와 라이브러리 · pnx and the library
sh docs/tools/build.sh spec             # 예제 → PDF → HTML → Markdown → 점검 · examples, PDF, HTML, Markdown, checks
sh docs/tools/build.sh spec quick check # 원고가 조판되는지만 · just check the manuscripts compile
```

[Typst](https://typst.app) 0.15 와 Noto Serif CJK KR, Noto Sans CJK KR, D2Coding 글꼴이 필요하다. `TYPST` 와
`TYPST_FONT_PATHS` 로 알려 준다. 글꼴을 못 찾으면 PDF 를 버린다 --- Typst 는 대체 글꼴로 조용히 성공하기 때문이다.
Needs Typst 0.15 and the fonts above (set `TYPST` and `TYPST_FONT_PATHS`); a PDF set in substituted fonts is discarded.

## 라이선스 · License

본문은 CC BY-NC-SA 4.0, 예제는 MIT 다. [`LICENSE-NOTICE.md`](LICENSE-NOTICE.md) 를 본다.
Text under CC BY-NC-SA 4.0, examples under MIT; see [`LICENSE-NOTICE.md`](LICENSE-NOTICE.md).

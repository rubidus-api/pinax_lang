# Pinax 표준 · The Pinax Standard

끼워 쓰는 스크립트 언어 Pinax 의 정의다. 무엇이 적합한 프로그램인지, 그것이 돌 때 무엇을 하는지, 처리기가 무엇을
거절하는지, C 호스트가 처리기를 어떻게 부르는지를 번호 붙은 조항으로 정한다. 한국어판이 원본이고 영어판은 같은 조항과
같은 문단 번호를 쓴다.

The definition of the embeddable scripting language Pinax, in numbered clauses: what a conforming program is, what it
does when it runs, what a processor refuses, and how a C host drives it. The Korean edition is the original; the English
edition has the same clauses and paragraph numbers.

| 폴더 · folder | 무엇 · what |
|---|---|
| [`md-ko/`](md-ko/README.md) · [`md-en/`](md-en/README.md) | GitHub 에서 읽는 Markdown 판(생성본) · the Markdown editions, generated |
| 웹판 · web | <https://rubidus-api.github.io/pinax_lang/spec/html-ko/> · <https://rubidus-api.github.io/pinax_lang/spec/html-en/> |
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
sh docs/tools/verify-examples.sh spec      # 예제마다 첫 줄의 약속 · each example against its first line
```

## 라이선스 · License

본문과 예제 모두 MIT 다. [`LICENSE`](LICENSE) 를 본다. 매뉴얼(`../manual/`)은 라이선스가 다르다.
Text and examples are both under the MIT License ([`LICENSE`](LICENSE)). The manual (`../manual/`) has a different license.

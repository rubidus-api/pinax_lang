# docs/tools

Builds the Pinax standard (`../spec/`) and manual (`../manual/`) from their Typst manuscripts, in Korean and English.
MIT (see `../manual/LICENSE-CODE`).

| file | does |
|---|---|
| `build.sh` | `sh docs/tools/build.sh spec\|manual [examples\|pdf\|html\|md\|check\|quick]` from the repository root |
| `verify-examples.sh` | runs every example of a tree with `build/native/pnx` and the library, captures the output |
| `common.typ` | the shared Typst functions (`#ex`, `#para`, `#term`, `#qa`, `#mistake`, `#diag` …) |
| `web.py` | splits typst's HTML into pages, writes the Markdown edition from the same HTML |
| `check.py` | editions in step, an example and a refusal where the rules need one, terms defined once, no local paths |
| `pinax.sublime-syntax`, `style.css` | code highlighting, the web edition's style |

# docs/tools

What the public tree keeps for the Pinax standard (`../spec/`) and manual (`../manual/`). MIT (see
`../manual/LICENSE-CODE`). The manuscripts and the tools that build the editions from them are not published; the web
editions and the PDFs are built at each release.

| file | does |
|---|---|
| `verify-examples.sh` | `sh docs/tools/verify-examples.sh spec\|manual`: runs every example of a tree with `build/native/pnx` and the library and checks it against its first line |
| `words.py` | writes `src/wasm/words.mjs`, the playground's word list, from the tables of the standard's Markdown edition (`../spec/md-en/`); `--check` fails when it is stale |
| `pinax.sublime-syntax` | code highlighting |

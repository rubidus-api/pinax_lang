#!/usr/bin/env python3
"""Checks of one document tree. MIT (see ../manual/LICENSE-CODE).

    check.py spec|manual        (from the repository root, after verify-examples.sh)

1. Editions: typst-ko and typst-en have the same files; each pair has the same headings by
   level, the same numbered paragraphs and the same examples in the same order. An English
   file whose first line holds TODO(en) is not compared yet; it is reported, and the check
   fails when PINAX_DOCS_FINAL=1.
2. Shape: in the spec every clause file that states constraints shows a refused example; in
   the manual every chapter has an opening, a running example and a refused or faulting one,
   unless the file says `// shape: <reason>` in its first lines.
3. Examples: every example is shown somewhere, and every example shown exists.
4. No local absolute path in any file of the tree.
5. Each term is defined (#term) once.
6. Every hand-typed reference N.M names a heading of the standard; a Korean term has its English
   twin at the same place; a chapter answers two questions and shows a mistake, a clause with
   faults shows one.
"""
import os
import pathlib
import re
import sys

tree = sys.argv[1]
root = pathlib.Path("docs") / tree
problems, pending = [], []


def outline(text):
    heads = [(len(m.group(1)), "") for m in re.finditer(r"^(=+) ", text, re.M)]
    paras = re.findall(r'#para\("([0-9a-z]+)"', text)
    exs = re.findall(r'#ex\("' + tree + r'", "([^"]+)"', text)
    return heads, paras, exs


ko = {p.relative_to(root / "typst-ko") for p in (root / "typst-ko").rglob("*.typ")}
en = {p.relative_to(root / "typst-en") for p in (root / "typst-en").rglob("*.typ")}
for p in sorted(ko ^ en):
    problems.append(f"editions: {p} is in one edition only")
for p in sorted(ko & en):
    k = (root / "typst-ko" / p).read_text(encoding="utf-8")
    e = (root / "typst-en" / p).read_text(encoding="utf-8")
    if "TODO(en)" in e.split("\n", 1)[0]:
        pending.append(str(p))
        continue
    a, b = outline(k), outline(e)
    for what, x, y in zip(("headings", "paragraphs", "examples"), a, b):
        if x != y:
            problems.append(f"editions: {p}: the {what} differ ({len(x)} in ko, {len(y)} in en)")

kinds = {}
out = root / "build" / "examples-out"
for f in out.glob("*.kind"):
    kinds[f.name[: -len(".kind")]] = f.read_text().strip()
shown = set()
for p in sorted(ko):
    text = (root / "typst-ko" / p).read_text(encoding="utf-8")
    _, _, exs = outline(text)
    shown |= set(exs)
    for x in exs:
        if not (root / "examples" / x).exists():
            problems.append(f"examples: {p} shows {x}, which does not exist")
    head = "\n".join(text.split("\n")[:5])
    if "// shape:" in head or p.parent.name not in ("clauses", "chapters"):
        continue
    ks = [kinds.get(x, "") for x in exs]
    bad = any(k in ("refused", "fault") for k in ks)
    good = any(k in ("run", "test", "repl", "embed", "fmt", "see") for k in ks)  # "module": source only
    if tree == "spec":
        if ("#part(\"제약\")" in text or "#part(tr(\"제약\"" in text) and not any(k == "refused" for k in ks):
            problems.append(f"shape: {p} states constraints but shows no refused example")
    else:
        if "#opening(" not in text and "#opening[" not in text:
            problems.append(f"shape: {p} has no #opening")
        if not good:
            problems.append(f"shape: {p} shows no running example")
        if not bad:
            problems.append(f"shape: {p} shows no refused or faulting example")
for f in sorted((root / "examples").glob("*")):
    if f.suffix in (".pnx", ".c") and f.name not in shown:
        head = f.read_text(encoding="utf-8").split("\n", 3)
        if not any(l.strip() == "rem hide" for l in head):
            problems.append(f"examples: {f.name} is shown nowhere")

# 5. A term is defined (#term) in one place only.
defined = {}
for p in sorted(ko):
    for m in re.finditer(r'#term\("([^"]+)"', (root / "typst-ko" / p).read_text(encoding="utf-8")):
        defined.setdefault(m.group(1), []).append(str(p))
for t, where in sorted(defined.items()):
    if len(where) > 1:
        problems.append(f"terms: {t} is defined in {len(where)} places: " + ", ".join(where))

# 6. Hand-typed references name a heading that exists. The numbering is computed as typst does
# it: level-1 headings in include order (annexes, numbered by letter, are skipped), level-2
# headings within each. A manual chapter cites the standard's clauses, so its references are
# checked against the standard's headings. Numbers inside code are not references.
def headings_of(tree_root, main_file):
    numbers = {}
    clause = 0

    def walk(path):
        nonlocal clause
        text = path.read_text(encoding="utf-8")
        for line in text.split("\n"):
            m = re.match(r"^(=+) ", line)
            if m and "annex" not in path.name:
                if len(m.group(1)) == 1:
                    clause += 1
                    numbers.setdefault(clause, 0)
                elif len(m.group(1)) == 2 and clause:
                    numbers[clause] = numbers.get(clause, 0) + 1
            inc = re.match(r'#include "([^"]+)"', line)
            if inc:
                walk((path.parent / inc.group(1)))
    walk(tree_root / main_file)
    return numbers


def references_in(text):
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"`[^`\n]*`", " ", text)
    text = re.sub(r"\$[^$\n]*\$", " ", text)  # math
    text = re.sub(r'\\"[^"]*\\"', " ", text)  # a quoted string in a trace row
    found = re.findall(r"(?<![\d.\w])(\d{1,2})\.(\d{1,2})(?![\d.\w%])", text)
    return [(int(a), int(b)) for a, b in found if int(a) > 0]  # 0.1 is a number, not a clause


spec_numbers = headings_of(pathlib.Path("docs/spec/typst-ko"), "main.typ")
for p in sorted(ko):
    if p.parent.name not in ("clauses", "chapters"):
        continue
    text = (root / "typst-ko" / p).read_text(encoding="utf-8")
    for a, b in references_in(text):
        if a not in spec_numbers or b == 0 or b > spec_numbers[a]:
            problems.append(f"references: {p} cites {a}.{b}, which is no clause of the standard")

# 7. Each term the Korean clause defines has its English twin at the same place of the English
# clause: the `en:` of the Korean #term, in order, is the list of English #term words.
for p in sorted(ko & en):
    k = (root / "typst-ko" / p).read_text(encoding="utf-8")
    e = (root / "typst-en" / p).read_text(encoding="utf-8")
    if "TODO(en)" in e.split("\n", 1)[0]:
        continue
    twins = []
    for m in re.finditer(r'#term\("([^"]+)"(?:,\s*en:\s*"([^"]+)")?\)', k):
        if not m.group(2):
            problems.append(f"terms: {p} defines {m.group(1)} without its English (en:)")
        twins.append(" ".join((m.group(2) or "?").split()))
    words = [" ".join(w.split()) for w in re.findall(r'#term\("([^"]+)"', e)]
    if twins != words:
        problems.append(f"terms: {p}: the Korean terms ({', '.join(twins)}) and the English ({', '.join(words)}) differ")

# 8. The shape a reader new to the language needs: a chapter has at least two questions
# answered and one common mistake; a clause with a faults part shows a faulting example.
# Reported, and a failure when PINAX_DOCS_SHAPE=1 (set once the chapters have that shape).
later = []  # kept for a rule that is being introduced; none now
shape = problems
for p in sorted(ko):
    if p.parent.name not in ("clauses", "chapters"):
        continue
    text = (root / "typst-ko" / p).read_text(encoding="utf-8")
    head = "\n".join(text.split("\n")[:5])
    if "// shape:" in head:
        continue
    if tree == "manual":
        if text.count("#qa[") < 2:
            shape.append(f"shape: {p} answers fewer than two questions (#qa)")
        if "#mistake[" not in text:
            shape.append(f"shape: {p} shows no common mistake (#mistake)")
    else:
        _, _, exs = outline(text)
        if '#part("결함")' in text and not any(kinds.get(x, "") == "fault" for x in exs):
            shape.append(f"shape: {p} states faults but shows no faulting example")

local = re.compile(r"/(opt|home|mnt)/[A-Za-z]")
for p in root.rglob("*"):
    if p.is_file() and "build" not in p.parts and p.suffix in (".typ", ".md", ".pnx", ".c", ".html", ".in"):
        if local.search(p.read_text(encoding="utf-8", errors="replace")):
            problems.append(f"privacy: {p} holds a local path")

for p in problems:
    print("  " + p)
if pending:
    print(f"  pending: {len(pending)} English file(s) marked TODO(en): " + ", ".join(pending))
if later:
    print(f"  later ({len(later)}, PINAX_DOCS_SHAPE=1 makes them problems): " + "; ".join(later))
final = os.environ.get("PINAX_DOCS_FINAL") == "1"
print(f"check {tree}: {len(problems)} problem(s), {len(pending)} pending")
sys.exit(1 if problems or (final and pending) else 0)

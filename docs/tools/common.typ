// Shared by the spec and the manual, both languages. MIT (see ../manual/LICENSE-CODE).
// Compiled with --root docs and --input lang=ko|en (and --features html for the web edition).

#let lang = sys.inputs.at("lang", default: "ko")
#let ko = lang == "ko"
#let tr(k, e) = if ko { k } else { e }

#let is-html() = target() == "html"

// A block with a class in HTML and a framed box on paper.
#let boxed(kind, title, body, fill: luma(246), stroke: luma(160)) = context {
  if is-html() {
    html.elem("div", attrs: (class: "box " + kind))[
      #if title != none { html.elem("p", attrs: (class: "box-title"))[#title] }
      #body
    ]
  } else {
    block(width: 100%, inset: (x: 9pt, y: 7pt), radius: 2pt, fill: fill,
          stroke: (left: 2pt + stroke), breakable: true, above: 0.9em, below: 0.9em)[
      #set par(first-line-indent: 0pt)
      #if title != none { text(font: ("Noto Sans CJK KR", "Noto Sans"), weight: "bold", size: 0.92em)[#title] ; parbreak() }
      #body
    ]
  }
}

// A question the reader is likely to ask, answered at once.
#let qa(q, a) = boxed("qa", tr("문답", "Q&A"))[
  #strong[#tr("물음", "Q"). #q]

  #tr("답", "A"). #a
]
// A note (informative; rationale, history, comparison).
#let note(body) = boxed("note", tr("참고", "Note"), body)
// A belief that is wrong, the evidence and the rule.
#let mistake(title, body) = boxed("mistake", tr("흔한 오해", "Common mistake") + ": " + title, body,
                                  fill: rgb("#fbf3f1"), stroke: rgb("#b3584a"))
// What a chapter builds on and what it answers.
#let opening(builds, questions) = boxed("opening", none)[
  #strong[#tr("먼저 알아야 할 것", "Builds on")]: #builds
  #parbreak()
  #strong[#tr("이 장이 답하는 물음", "Questions this chapter answers")]
  #questions
]
// The rules a chapter introduced, each with its spec clause.
#let summary(body) = boxed("summary", tr("정리", "Summary"), body)

// A term at its defining place: bold, with the English word in the Korean edition.
#let term(word, en: none) = {
  let shown = if ko and en != none [*#word*(#en)] else [*#word*]
  [#shown#metadata((term: word, en: en))<term>]
}

// Spec paragraphs: (n) with a hanging number. `n` is a string so that "3a" works.
#let para(n, body) = context {
  if is-html() {
    html.elem("p", attrs: (class: "para"))[#html.elem("span", attrs: (class: "pn"))[(#n)] #body]
  } else {
    block(above: 0.75em, below: 0.75em)[
      #set par(first-line-indent: 0pt, hanging-indent: 2.2em)
      #box(width: 2.2em)[(#n)]#body
    ]
  }
}

// The parts of a clause (Syntax, Constraints, Semantics, ...): an unnumbered small heading.
#let part(name) = heading(level: 4, numbering: none, outlined: false)[#name]

// A diagnostics table: pairs of (condition, message).
#let diag(..rows) = {
  let cells = rows.pos().map(r => (r.at(0), raw(r.at(1)))).flatten()
  table(columns: (1fr, 1.3fr), inset: 5pt, stroke: 0.5pt + luma(170),
        table.header([*#tr("조건", "Condition")*], [*#tr("메시지", "Message")*]), ..cells)
}

// Kinds of example outputs and their labels.
#let out-label(kind) = (
  run: tr("출력", "Output"), test: tr("시험 결과", "Test run"),
  refused: tr("거절", "Refused"), fault: tr("결함으로 멈춤", "Fault"),
  repl: tr("대화", "Session"), fmt: tr("정리한 모양", "Formatted"),
  see: tr("목록", "Listing"), embed: tr("출력", "Output"),
).at(kind, default: kind)

// An example: the program from <tree>/examples/<name> and what running it printed, both read
// from what verify-examples.sh wrote (the program without its contract lines, the output with
// the command line first). Nothing here is typed by hand.
#let ex(tree, name, caption: none) = {
  let base = "/" + tree + "/build/examples-out/" + name
  let src = read(base + ".src")
  let out = read(base + ".out")
  let kind = read(base + ".kind").trim()
  let lang-of = if name.ends-with(".c") { "c" } else { "pinax" }
  context {
    if is-html() {
      html.elem("div", attrs: (class: "example " + kind))[
        #html.elem("p", attrs: (class: "ex-file"))[#raw(name)#if caption != none [ --- #caption]]
        #raw(src, block: true, lang: lang-of)
        #html.elem("p", attrs: (class: "ex-label"))[#out-label(kind)]
        #raw(out, block: true, lang: "text")
      ]
    } else {
      block(width: 100%, breakable: true, above: 1em, below: 1em)[
        #set par(first-line-indent: 0pt)
        #text(font: ("Noto Sans CJK KR", "Noto Sans"), size: 0.85em, fill: luma(90))[#raw(name)#if caption != none [ --- #caption]]
        #block(width: 100%, inset: 7pt, fill: luma(248), stroke: 0.5pt + luma(190), radius: 2pt,
               breakable: true)[#raw(src, block: true, lang: lang-of)]
        #v(-0.4em)
        #text(font: ("Noto Sans CJK KR", "Noto Sans"), size: 0.85em, fill: if kind in ("refused", "fault") { rgb("#9c3b2e") } else { luma(90) })[#out-label(kind)]
        #block(width: 100%, inset: 7pt, fill: if kind in ("refused", "fault") { rgb("#fbf3f1") } else { white },
               stroke: 0.5pt + luma(190), radius: 2pt, breakable: true)[#raw(out, block: true, lang: "text")]
      ]
    }
  }
}

// The whole document: page, fonts, headings, title page and contents.
#let document-setup(title: "", subtitle: "", version: "", date: "", body) = {
  set document(title: title + " " + version, author: "rubidus-api")
  set text(font: ("Noto Serif CJK KR", "Noto Serif"), size: 10.5pt, lang: lang)
  set par(justify: true, leading: 0.78em)
  set heading(numbering: "1.1")
  show heading: set text(font: ("Noto Sans CJK KR", "Noto Sans"))
  show heading.where(level: 4): set text(size: 0.95em, fill: luma(60))
  show raw: set text(font: "D2Coding", ligatures: false)
  // D2Coding at typst's raw size looks a size smaller than the text beside it.
  show raw.where(block: false): set text(size: 1.18em)
  show raw.where(block: true): set text(size: 1.1em)
  show raw.where(block: true, lang: "ebnf"): it => context {
    if is-html() { it } else {
      block(width: 100%, inset: (x: 8pt, y: 6pt), fill: rgb("#f4f6fa"), stroke: (left: 2pt + rgb("#8a9bb8")),
            breakable: true, it)
    }
  }
  set raw(syntaxes: "/tools/pinax.sublime-syntax")
  show link: it => if type(it.dest) == str { text(fill: rgb("#1A4F8A"), it) } else { it }

  // Paper only: the page, the running head and a new page for each top-level heading.
  let paged(body) = {
    set page(paper: "a4", margin: (x: 2.3cm, y: 2.4cm), numbering: "1",
             header: context {
               if counter(page).get().first() > 1 {
                 set text(font: ("Noto Sans CJK KR", "Noto Sans"), size: 8pt, fill: luma(110))
                 grid(columns: (1fr, 1fr), align(left)[#title], align(right)[#version])
               }
             })
    show heading.where(level: 1): it => { pagebreak(weak: true); it }
    body
  }
  show: body => context if is-html() { body } else { paged(body) }

  context if is-html() {
    html.elem("h1", attrs: (class: "doc-title"))[#title]
    html.elem("p", attrs: (class: "doc-sub"))[#subtitle --- #version, #date]
  } else {
    v(30%)
    align(center)[
      #text(font: ("Noto Sans CJK KR", "Noto Sans"), size: 26pt, weight: "bold")[#title] \
      #v(0.6em)
      #text(size: 13pt)[#subtitle] \
      #v(2em)
      #text(size: 11pt)[#version --- #date]
    ]
    pagebreak()
    outline(depth: 2)
  }
  body
}

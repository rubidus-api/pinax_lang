#!/usr/bin/env python3
"""The web and Markdown editions, from the one HTML file typst writes. MIT (../manual/LICENSE-CODE).

    web.py html|md TREE LANG INPUT.html OUTDIR

The document is split at each top-level heading (an <h2> in typst's HTML) into one page per
chapter or clause: OUTDIR/index.html (or README.md) holds the title page and the contents, and
NN.html (NN.md) one part each. The Markdown edition is a translation of the same HTML, so the
two cannot disagree with the PDF about the text.
"""
import html
import pathlib
import re
import shutil
import sys
from html.parser import HTMLParser

VOID = {"br", "img", "hr", "meta", "link", "input", "col"}
HERE = pathlib.Path(__file__).resolve().parent


class Node:
    def __init__(self, tag, attrs, parent=None):
        self.tag, self.attrs, self.parent, self.kids = tag, dict(attrs), parent, []

    def cls(self):
        return (self.attrs.get("class") or "").split()

    def text(self):
        return "".join(k if isinstance(k, str) else k.text() for k in self.kids)


class Tree(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node("root", [])
        self.cur = self.root

    def handle_starttag(self, tag, attrs):
        n = Node(tag, attrs, self.cur)
        self.cur.kids.append(n)
        if tag not in VOID:
            self.cur = n

    def handle_startendtag(self, tag, attrs):
        self.cur.kids.append(Node(tag, attrs, self.cur))

    def handle_endtag(self, tag):
        n = self.cur
        while n is not self.root and n.tag != tag:
            n = n.parent
        if n is not self.root:
            self.cur = n.parent

    def handle_data(self, data):
        self.cur.kids.append(data)


def find(node, tag):
    if isinstance(node, str):
        return None
    if node.tag == tag:
        return node
    for k in node.kids:
        r = find(k, tag)
        if r is not None:
            return r
    return None


def to_html(node):
    if isinstance(node, str):
        return html.escape(node, quote=False)
    attrs = "".join(f' {k}="{html.escape(v or "")}"' for k, v in node.attrs.items())
    if node.tag in VOID:
        return f"<{node.tag}{attrs}>"
    return f"<{node.tag}{attrs}>" + "".join(to_html(k) for k in node.kids) + f"</{node.tag}>"


def split(body):
    """Front matter, then (title, id, nodes) per top-level heading."""
    front, parts = [], []
    for k in body.kids:
        if not isinstance(k, str) and k.tag == "h2":
            parts.append((k.text().strip(), k.attrs.get("id", ""), [k]))
        elif parts:
            parts[-1][2].append(k)
        else:
            front.append(k)
    return front, parts


def page_names(parts):
    return [f"{i + 1:02d}" for i in range(len(parts))]


def anchors(parts, names, ext):
    """Every id in a part points to that part's page."""
    where = {}

    def walk(n, page):
        if isinstance(n, str):
            return
        if n.attrs.get("id"):
            where[n.attrs["id"]] = page
        for k in n.kids:
            walk(k, page)

    for (_, _, nodes), name in zip(parts, names):
        for n in nodes:
            walk(n, name + ext)
    return where


def relink(node, where):
    if isinstance(node, str):
        return
    if node.tag == "a" and (node.attrs.get("href") or "").startswith("#"):
        frag = node.attrs["href"][1:]
        if frag in where:
            node.attrs["href"] = where[frag] + "#" + frag
    for k in node.kids:
        relink(k, where)


# ---- HTML edition -----------------------------------------------------------------------

def write_html(tree, lang, doc, out):
    body = find(doc, "body")
    title_node = next((k for k in body.kids if not isinstance(k, str) and "doc-title" in k.cls()), None)
    title = title_node.text().strip() if title_node else tree
    front, parts = split(body)
    names = page_names(parts)
    where = anchors(parts, names, ".html")
    for k in body.kids:
        relink(k, where)
    label = {"ko": ("차례", "앞", "다음", "처음"), "en": ("Contents", "Previous", "Next", "Start")}[lang]
    other = "en" if lang == "ko" else "ko"

    def page(head, content, nav):
        return (f'<!DOCTYPE html>\n<html lang="{lang}"><head><meta charset="utf-8">'
                f'<meta name="viewport" content="width=device-width, initial-scale=1">'
                f"<title>{html.escape(head)}</title><link rel=\"stylesheet\" href=\"style.css\"></head>"
                f'<body><nav class="top">{nav}</nav><main>{content}</main><nav class="bottom">{nav}</nav></body></html>\n')

    toc = "<ol class=\"toc\">" + "".join(
        f'<li><a href="{n}.html">{html.escape(t)}</a></li>' for (t, _, _), n in zip(parts, names)) + "</ol>"
    lang_link = f'<a href="../html-{other}/index.html">{"English" if lang == "ko" else "한국어"}</a>'
    (out / "index.html").write_text(page(title, "".join(to_html(k) for k in front) + f"<h2>{label[0]}</h2>" + toc,
                                         lang_link), encoding="utf-8")
    for i, ((t, _, nodes), n) in enumerate(zip(parts, names)):
        nav = [f'<a href="index.html">{label[3]}</a>']
        if i > 0:
            nav.append(f'<a href="{names[i - 1]}.html">{label[1]}</a>')
        if i + 1 < len(parts):
            nav.append(f'<a href="{names[i + 1]}.html">{label[2]}</a>')
        nav.append(f'<a href="../html-{other}/{n}.html">{"English" if lang == "ko" else "한국어"}</a>')
        (out / f"{n}.html").write_text(page(f"{t} - {title}", "".join(to_html(k) for k in nodes), " · ".join(nav)),
                                       encoding="utf-8")
    shutil.copy(HERE / "style.css", out / "style.css")


# ---- Markdown edition -------------------------------------------------------------------

def inline(node):
    if isinstance(node, str):
        return re.sub(r"\s+", " ", node)
    t = node.tag
    inner = "".join(inline(k) for k in node.kids)
    if t == "strong":
        return f"**{inner.strip()}**" if inner.strip() else ""
    if t == "em":
        return f"*{inner.strip()}*" if inner.strip() else ""
    if t == "code":
        s = node.text()
        fence = "``" if "`" in s else "`"
        return f"{fence}{s}{fence}"
    if t == "a":
        return f"[{inner.strip()}]({node.attrs.get('href', '')})"
    if t == "br":
        return "  \n"
    return inner


def block(node, depth=0):
    """Markdown lines for a block node."""
    if isinstance(node, str):
        s = node.strip()
        return [re.sub(r"\s+", " ", s)] if s else []
    t, c = node.tag, node.cls()
    if t in ("h2", "h3", "h4", "h5", "h6"):
        return ["#" * (int(t[1]) - 1) + " " + node.text().strip(), ""]
    if t == "p" and "para" in c:
        return [inline(node).strip(), ""]
    if t == "p":
        s = inline(node).strip()
        return [s, ""] if s else []
    if t == "pre":
        code = find(node, "code")
        lang = (code.attrs.get("data-lang") if code else "") or ""
        return ["```" + lang] + node.text().rstrip("\n").split("\n") + ["```", ""]
    if t in ("ul", "ol"):
        lines = []
        for i, li in enumerate(k for k in node.kids if not isinstance(k, str) and k.tag == "li"):
            mark = "-" if t == "ul" else f"{i + 1}."
            sub = []
            for k in li.kids:
                sub += block(k, depth + 1) if not isinstance(k, str) and k.tag in ("ul", "ol", "pre", "p", "div") else [inline(k)]
            text = " ".join(s for s in sub if s and not s.startswith(("```", "- ", "  "))).strip()
            lines.append(mark + " " + re.sub(r"\s+", " ", text))
        return lines + [""]
    if t == "table":
        rows = []
        for tr in iter_tag(node, "tr"):
            rows.append([inline(td).strip().replace("|", "\\|") for td in tr.kids if not isinstance(td, str)])
        if not rows:
            return []
        width = max(len(r) for r in rows)
        rows = [r + [""] * (width - len(r)) for r in rows]
        lines = ["| " + " | ".join(rows[0]) + " |", "|" + "---|" * width]
        lines += ["| " + " | ".join(r) + " |" for r in rows[1:]]
        return lines + [""]
    if t == "div" and "example" in c:
        lines = []
        for k in node.kids:
            if isinstance(k, str):
                continue
            if "ex-file" in k.cls():
                lines += [inline(k).strip(), ""]
            elif "ex-label" in k.cls():
                lines += ["*" + k.text().strip() + "*", ""]
            else:
                lines += block(k)
        return lines
    if t == "div" and "box" in c:
        inner = []
        for k in node.kids:
            if not isinstance(k, str) and "box-title" in k.cls():
                inner += ["**" + k.text().strip() + "**", ""]
            else:
                inner += block(k) if not isinstance(k, str) and k.tag not in ("strong", "em", "code", "a", "span", "br") else [inline(k)]
        joined = "\n".join(inner).strip("\n").split("\n")
        return ["> " + s if s else ">" for s in joined] + [""]
    if t in ("div", "section", "figure", "main", "span", "li"):
        lines = []
        pending = ""
        for k in node.kids:
            if isinstance(k, str) or k.tag in ("strong", "em", "code", "a", "br", "span"):
                pending += inline(k)
            else:
                if pending.strip():
                    lines += [pending.strip(), ""]
                pending = ""
                lines += block(k, depth)
        if pending.strip():
            lines += [pending.strip(), ""]
        return lines
    if t == "figcaption":
        return ["*" + inline(node).strip() + "*", ""]
    return [inline(node).strip(), ""] if inline(node).strip() else []


def iter_tag(node, tag):
    if isinstance(node, str):
        return
    if node.tag == tag:
        yield node
        return
    for k in node.kids:
        yield from iter_tag(k, tag)


def write_md(tree, lang, doc, out):
    body = find(doc, "body")
    front, parts = split(body)
    names = page_names(parts)
    where = anchors(parts, names, ".md")
    for k in body.kids:
        relink(k, where)
    label = {"ko": ("차례", "앞", "다음", "처음"), "en": ("Contents", "Previous", "Next", "Start")}[lang]

    def tidy(lines):
        text = "\n".join(lines)
        return re.sub(r"\n{3,}", "\n\n", text).strip() + "\n"

    lines = []
    for k in front:
        if not isinstance(k, str) and "doc-title" in k.cls():
            lines += ["# " + k.text().strip(), ""]
        else:
            lines += block(k)
    lines += ["## " + label[0], ""] + [f"{i + 1}. [{t}]({n}.md)" for i, ((t, _, _), n) in enumerate(zip(parts, names))]
    (out / "README.md").write_text(tidy(lines), encoding="utf-8")
    for i, ((t, _, nodes), n) in enumerate(zip(parts, names)):
        nav = [f"[{label[3]}](README.md)"]
        if i > 0:
            nav.append(f"[{label[1]}]({names[i - 1]}.md)")
        if i + 1 < len(parts):
            nav.append(f"[{label[2]}]({names[i + 1]}.md)")
        lines = []
        for k in nodes:
            lines += block(k)
        lines += ["---", "", " · ".join(nav)]
        (out / f"{n}.md").write_text(tidy(lines), encoding="utf-8")


def main():
    mode, tree, lang, src, out = sys.argv[1:6]
    parser = Tree()
    parser.feed(pathlib.Path(src).read_text(encoding="utf-8"))
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (write_html if mode == "html" else write_md)(tree, lang, parser.root, out)


if __name__ == "__main__":
    main()

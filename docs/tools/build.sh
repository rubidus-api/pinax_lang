#!/bin/sh
# Builds one document tree in both languages. MIT (see ../manual/LICENSE-CODE).
#
#   sh docs/tools/build.sh spec|manual [pdf|html|md|quick]...   (from the repository root)
#
# With no step: examples, PDF, HTML, Markdown and the checks. `quick` checks that the
# manuscripts compile (PDF only, not kept). Needs ./nob (for pnx), python3 and typst 0.15 with
# the fonts Noto Serif CJK KR, Noto Sans CJK KR and D2Coding; set TYPST to the typst program and
# TYPST_FONT_PATHS to the font folders when they are not found otherwise.
#
# A missing font does not stop typst: it sets the text in another font and succeeds. This
# script reads typst's log and throws the PDF away when that happened.
set -eu
tree=${1:?usage: build.sh spec|manual [pdf|html|md|quick]...}
shift
steps=${*:-examples pdf html md check}
root=$(pwd)
docs=$root/docs
dir=$docs/$tree
typst=${TYPST:-typst}
command -v "$typst" >/dev/null 2>&1 || { echo "build: typst not found (set TYPST)" >&2; exit 2; }
mkdir -p "$dir/build"

compile() { # lang format output
    log=$dir/build/typst-$1-$2.log
    if [ "$2" = html ]; then
        "$typst" compile --root "$docs" --input "lang=$1" --features html --format html \
            "$dir/typst-$1/main.typ" "$3" >"$log" 2>&1 || { cat "$log" >&2; exit 1; }
    else
        "$typst" compile --root "$docs" --input "lang=$1" "$dir/typst-$1/main.typ" "$3" >"$log" 2>&1 \
            || { cat "$log" >&2; exit 1; }
    fi
    if grep -q "unknown font family" "$log"; then
        cat "$log" >&2
        echo "build: a font was not found, so $3 is set in another font; thrown away" >&2
        rm -f "$3"
        exit 1
    fi
    grep -v "html export is under active development\|its behaviour may change\|do not rely on this\|issues/5512\|^\s*$" "$log" \
        | grep . >&2 || true
}

for step in $steps; do
    case $step in
        examples) sh "$docs/tools/verify-examples.sh" "$tree" ;;
        quick)
            for l in ko en; do compile $l pdf "$dir/build/quick-$l.pdf"; done
            echo "build $tree: manuscripts compile" ;;
        pdf)
            for l in ko en; do
                mkdir -p "$dir/pdf-$l"
                compile $l pdf "$dir/pdf-$l/pinax-$tree-$l.pdf"
                echo "build $tree: pdf-$l/pinax-$tree-$l.pdf"
            done ;;
        html)
            for l in ko en; do
                rm -rf "$dir/html-$l"
                mkdir -p "$dir/html-$l"
                compile $l html "$dir/build/$tree-$l.html"
                python3 "$docs/tools/web.py" html "$tree" "$l" "$dir/build/$tree-$l.html" "$dir/html-$l"
                echo "build $tree: html-$l/"
            done ;;
        md)
            for l in ko en; do
                [ -f "$dir/build/$tree-$l.html" ] || compile $l html "$dir/build/$tree-$l.html"
                rm -rf "$dir/md-$l"
                mkdir -p "$dir/md-$l"
                python3 "$docs/tools/web.py" md "$tree" "$l" "$dir/build/$tree-$l.html" "$dir/md-$l"
                echo "build $tree: md-$l/"
            done ;;
        check) python3 "$docs/tools/check.py" "$tree" ;;
        *) echo "build: unknown step $step" >&2; exit 2 ;;
    esac
done

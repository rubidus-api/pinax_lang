// console.mjs - the terminal of the Pinax playground, without the page: a session, and a program
// kept beside it as GW-BASIC keeps one: lines with numbers, which the numbers do not change. The
// page (playground.html) draws it.
//
//   import { createConsole } from "./console.mjs";
//   const con = await createConsole({ createPinax, bytes, print, store });
//   con.enter("3 4 add.");   // { prompt: "pinax> " }
//
// print(kind, text) receives what is to be shown: "echo" (a line as typed, with its prompt), "out"
// and "err" (what Pinax writes on streams 1 and 2), "stack" (the values left), "info" (what a
// command says). store, when given, keeps the program between visits: { get(): text or null,
// set(text) }.
//
// A line that begins with ":" is a command. Other lines gather until the text is whole: it ends
// with "," or ".", or with the `end` of the definition or block it opened, and no definition or
// block is open in it. Then it runs as one line of the session: a loop or a block runs once, when
// it is all typed. A statement that succeeds and begins with a declaration (word, const, global,
// use, set_order and the others) is added to the program; one that declares a name the program
// declares already takes the place of those lines. :edit N and :insert N change lines by number
// (:insert N above line N, :insert 0 at the end).
// enter() gives { prompt }, and for some commands what the page is to do: { fill } (text for the
// prompt), { editor } (the program to edit whole), { download }, { upload }. status() says what
// the terminal waits for: the definitions and blocks open, the line being edited or inserted.
// MIT (see ../../LICENSE).

const DECLARATIONS = new Set(["word", "const", "global", "array", "list", "pool", "queue", "buffer", "tasks",
    "record", "type", "extern", "forward", "index", "bound", "shared", "test", "use", "set_order"]);
// statements whose end is plain from their form: `use NAME [as NAME]`, `set_order (...)`, `get_order`. They take the
// next name or one parenthesized list, so the terminal runs them when that is typed; a dot may be written, or not
const SELF_ENDED = /^\s*(use\s+[A-Za-z_][A-Za-z_0-9]*(\s+as\s+[A-Za-z_][A-Za-z_0-9]*)?|set_order\s*\([^()]*\)|get_order)\s*$/;
const PROMPT = "pinax> ", MORE = "  ...> ";
// what :help says and the page's guide shows: one list for both
export const GUIDE = {
    rules: [
        'Pinax code runs when it is whole: it ends with "," or ".", or with the end of its block.',
        "`use io`, `set_order (io, str)` and `get_order` need no dot: what they take is plain, so they run when typed.",
        "An empty line runs the text gathered, outside any block: the way out of a forgotten dot.",
        "A loop or a block runs once, when its end is typed; the status line shows the blocks open.",
        "A statement that declares something (word, const, global, use, set_order, ...) is added to the program.",
    ],
    commands: [
        [":list [N[-M]]", "the program with its line numbers"],
        [":edit N", "line N into the prompt; Enter puts it back and runs the program again"],
        [":edit", "the whole program in an editor (F2; Ctrl+S saves and runs it, Esc leaves)"],
        [":insert N", "new lines above line N (0: at the end); an empty line ends, and the program runs (:ins)"],
        [":delete N[-M]", "removes lines N to M (:del)"],
        [":run", "RESETS: empties the stack and forgets every definition, then runs the program from line 1 (F5); the program's lines stay"],
        [":new", "no program, and a fresh state"],
        [":clear", "empties the stack only"],
        [":words", "the words of the program, each with its signature"],
        [":get_order", "the search order as it is, as the word get_order tells it"],
        [":save, :load", "the program as a .pnx file"],
        [":help", "this list"],
    ],
    keys: [
        ["Enter", "enters the line"], ["Shift+Enter", "a new line without entering"],
        ["Tab", "writes the word offered; words are offered as you type, in the prompt and in the editor"],
        ["Alt+1 to Alt+9", "write the nth word offered"], ["Alt+Up, Alt+Down", "choose among the words (Alt+Down offers them)"],
        ["Up, Down", "walk the history"], ["Esc", "drops the lines gathered, or ends :edit N and :insert N"],
        ["F5", ":run"], ["F2", ":edit"], ["F1", "shows or hides this guide"], ["Ctrl+L", "clears the screen"],
    ],
};
const width = Math.max(...GUIDE.commands.map(([c]) => c.length)) + 2;
const HELP = [...GUIDE.rules,
    ...GUIDE.commands.map(([c, what]) => `  ${c.padEnd(width)}${what}`),
    "Keys:", ...GUIDE.keys.map(([k, what]) => `  ${k.padEnd(width)}${what}`)].join("\n");

// a call to try for a word just defined, from its signature "(n int, s str gives r int)": a value of each
// parameter's type, then the name, and a dot to run it; null when a type has no sample here
const SAMPLES = { int: "0", bool: "false", str: '""' };
export function sampleCall(name, signature) {
    const params = (signature ?? "()").replace(/^\(|\)$/g, "").split(/\bgives\b/)[0].split(",").map((p) => p.trim()).filter(Boolean);
    const values = params.map((p) => SAMPLES[p.split(/\s+/).pop()]);
    return values.includes(undefined) ? null : [...values, name].join(" ") + ".";
}
// the modules a text names with `use`, for completion
export function usedModules(text) {
    return [...new Set([...text.matchAll(/^[ \t]*use[ \t]+([A-Za-z_][A-Za-z_0-9]*)/gm)].map((m) => m[1]))];
}
// the words a program declares, for completion: [module "", word, signature, meaning, true]
export function userWords(program) {
    const out = new Map();
    for (const m of program.matchAll(/^[ \t]*(?:export[ \t]+|redefine[ \t]+)?word[ \t]+([A-Za-z_][A-Za-z_0-9]*)[ \t]*(\([^)]*\))?/gm))
        out.set(m[1], ["", m[1], m[2] ?? "()", "defined in this session", true]);
    return [...out.values()];
}

// the declaration a text begins with, as "word sq", "use io"; "" for set_order; null for none
function declared(text) {
    const words = text.replace(/^(\s*(rem\b[^\n]*)?\n)+/, "").trim().split(/[\s(]+/);
    let k = 0;
    if (words[k] === "export" || words[k] === "redefine")
        ++k;
    if (!DECLARATIONS.has(words[k]))
        return null;
    return words[k] === "set_order" ? "" : `${words[k]} ${words[k + 1] ?? ""}`;
}

export async function createConsole({ createPinax, bytes, print, store = null }) {
    let offset = 0; // while the program runs: the line before the part running, for the places said
    const placed = (text) => (offset ? text.replace(/^input:(\d+):/gm, (_, n) => `input:${Number(n) + offset}:`) : text);
    const pinax = await createPinax(bytes, {
        write: (stream, text) => (trialLog ? trialLog.push(text) : print(stream === 1 ? "out" : "err", placed(text))),
    });
    const session = pinax.session("input");
    let lines = [];      // the program
    let gathered = [];   // the lines of the text not yet run
    let trialLog = null; // while a word is checked: what Pinax says, kept to be told or not
    let trialled = false; // a word was checked and may stand half written in the session
    let mode = null;     // { kind: "edit", at } or { kind: "insert", at, first, added }: at is a line index
    const history = [];

    const keep = () => store && store.set(lines.join("\n"));
    const showStack = (stack) => stack.length && print("stack", stack.join(" ") + "\n");
    const plural = (n, one) => `${n} ${one}${n === 1 ? "" : "s"}`;
    const ends = (text) => /[,.]\s*$/.test(text);
    // the text is whole: nothing open, and it ends with , or . or with the end of what it opened (a
    // do block excepted: a word after it takes it)
    function whole(text) {
        const b = pinax.blocks(text);
        return b.ok && b.depth === 0 && (ends(text) || b.closed === 1);
    }
    // the program in the parts it runs in, as if typed: [{ start, end }], line indexes, end not in it
    function parts() {
        const out = [];
        let start = 0;
        for (let k = 0; k < lines.length; ++k) {
            const text = lines.slice(start, k + 1).join("\n");
            if (text.trim() === "")
                continue;
            if (whole(text)) {
                out.push({ start, end: k + 1 });
                start = k + 1;
            }
        }
        if (lines.slice(start).join("\n").trim() !== "")
            out.push({ start, end: lines.length });
        return out;
    }
    // "3" or "3-5" as line indexes [from, to), within 1..lines.length; null when it is not
    function range(arg) {
        const m = /^(\d+)(?:-(\d+))?$/.exec(arg ?? "");
        if (!m)
            return null;
        const from = Number(m[1]), to = Number(m[2] ?? m[1]);
        return from >= 1 && from <= to && to <= lines.length ? [from - 1, to] : null;
    }
    const noLine = (arg) => print("err", `no line ${arg ?? ""}: the program has ${plural(lines.length, "line")}; :list shows them\n`);

    // a fresh state, then the program from its first line; it stops at the first part that fails
    function runProgram() {
        session.reset();
        for (const { start, end } of parts()) {
            offset = start;
            const r = session.eval(lines.slice(start, end).join("\n"));
            offset = 0;
            if (!r.ok) {
                const at = end - start === 1 ? `line ${start + 1}` : `lines ${start + 1}-${end}`;
                print("info", r.unfinished ? `${at}: a definition or a block is not closed by the program's end\n`
                                           : `the program stops in ${at} (:list ${at.split(" ")[1]})\n`);
                return false;
            }
        }
        showStack(session.stack());
        return true;
    }

    // a statement typed that declares something: added, or in place of the lines declaring the same
    function remember(text) {
        const key = declared(text);
        if (key === null)
            return;
        const at = key === "" ? undefined : parts().find(({ start, end }) => declared(lines.slice(start, end).join("\n")) === key);
        if (at)
            lines.splice(at.start, at.end - at.start, ...text.split("\n"));
        else
            lines.push(...text.split("\n"));
        keep();
    }

    function command(line) {
        const [name, arg] = line.slice(1).trim().split(/\s+/, 2);
        switch (name) {
        case "help":
            print("info", HELP + "\n");
            return {};
        case "list": {
            if (lines.length === 0) {
                print("info", "the program is empty\n");
                return {};
            }
            const r = arg === undefined ? [0, lines.length] : range(arg);
            if (!r)
                return noLine(arg), {};
            const width = String(lines.length).length;
            let text = "";
            for (let k = r[0]; k < r[1]; ++k)
                text += `${String(k + 1).padStart(width + 2)}  ${lines[k]}\n`;
            print("info", text);
            return {};
        }
        case "edit": {
            if (arg === undefined)
                return { editor: lines.join("\n") };
            const r = range(arg);
            if (!r || r[1] - r[0] !== 1)
                return noLine(arg), {};
            mode = { kind: "edit", at: r[0] };
            return { fill: lines[r[0]] };
        }
        case "insert": case "ins": {
            const n = Number(arg);
            if (!/^\d+$/.test(arg ?? "") || n > lines.length) {
                print("err", `:insert N puts lines above line N: ${lines.length ? `1 to ${lines.length}, or ` : ""}0 for the end\n`);
                return {};
            }
            mode = { kind: "insert", at: n === 0 ? lines.length : n - 1, first: n, added: 0 };
            return {};
        }
        case "delete": case "del": {
            const r = range(arg);
            if (!r)
                return noLine(arg), {};
            lines.splice(r[0], r[1] - r[0]);
            keep();
            print("info", `${r[1] - r[0] === 1 ? `line ${r[0] + 1}` : `lines ${r[0] + 1}-${r[1]}`} removed; ` +
                          "the program runs again from a fresh state\n");
            runProgram();
            return {};
        }
        case "run":
            print("info", "reset: the stack is emptied and every definition forgotten; the program runs from line 1\n");
            runProgram();
            return {};
        case "new":
            lines = [];
            keep();
            session.reset();
            print("info", "no program; a fresh state\n");
            return {};
        case "clear":
            session.clear();
            print("info", "the stack is empty\n");
            return {};
        case "words": { // the words of the program, each with its signature
            const mine = userWords(lines.join("\n"));
            if (!mine.length)
                print("info", "no word is defined yet\n");
            else {
                const wide = Math.max(...mine.map((w) => w[1].length));
                print("info", mine.map((w) => `  ${w[1].padEnd(wide)}  ${w[2]}`).join("\n") + "\n");
            }
            return {};
        }
        case "get_order": { // what the word get_order tells, as a command
            const r = session.eval("get_order");
            return r.ok ? {} : {};
        }
        case "save":
            return { download: lines.join("\n") + "\n" };
        case "load":
            return { upload: true };
        default:
            print("err", `no command :${name ?? ""}; :help lists them\n`);
            return {};
        }
    }

    function prompt() {
        if (mode?.kind === "edit")
            return `edit ${mode.at + 1}> `;
        if (mode?.kind === "insert")
            return `insert ${mode.at + 1}> `;
        return gathered.length ? MORE : PROMPT;
    }

    const where = (first) => (first === 0 ? "at the end" : `above line ${first}`);
    // the end of an :insert: the program runs again when lines were added
    function endInsert() {
        const { first, added } = mode;
        mode = null;
        if (added === 0)
            return print("info", "nothing inserted\n");
        print("info", `${plural(added, "line")} inserted ${where(first)}; the program runs again from a fresh state\n`);
        runProgram();
    }

    function enter(line) {
        print("echo", prompt() + line + "\n");
        if (line.trim() !== "")
            history.push(line);
        if (mode?.kind === "edit") { // the text takes line N's place; it may be several lines
            const at = mode.at;
            mode = null;
            if (line.trim() === "" || line === lines[at]) {
                print("info", `line ${at + 1} unchanged\n`);
                return { prompt: prompt() };
            }
            lines.splice(at, 1, ...line.split("\n"));
            keep();
            print("info", `line ${at + 1} replaced; the program runs again from a fresh state\n`);
            runProgram();
            return { prompt: prompt() };
        }
        if (mode?.kind === "insert") {
            if (line.trim() === "" || line.trimStart().startsWith(":")) {
                endInsert();
                if (line.trim() === "")
                    return { prompt: prompt() };
            } else {
                lines.splice(mode.at++, 0, line);
                ++mode.added;
                keep();
                return { prompt: prompt() };
            }
        }
        if (gathered.length === 0 && line.trim() === "")
            return { prompt: prompt() };
        if (gathered.length === 0 && line.trimStart().startsWith(":"))
            return { ...command(line.trim()), prompt: prompt() };
        // an empty line after text that waits for its dot runs the text, when no block is open: the way out of a
        // forgotten dot (inside a block an empty line is only a line)
        if (gathered.length && line.trim() === "") {
            const text = gathered.join("\n");
            const b = pinax.blocks(text);
            if (!b.ok || b.depth > 0)
                return { prompt: prompt() };
            const r = session.eval(text);
            if (r.unfinished) { // the text itself is not whole (a statement that wants more): it still waits
                print("info", "(not a whole statement yet: it needs more)\n");
                return { prompt: prompt() };
            }
            gathered = [];
            trialled = false;
            if (r.ok) {
                remember(text);
                showStack(r.stack);
            }
            return { prompt: prompt() };
        }
        gathered.push(line);
        const text = gathered.join("\n");
        if (!whole(text) && !SELF_ENDED.test(text)) {
            if (ends(line.trimEnd()) && checkWord(text) === false) {
                gathered.pop(); // the line with the mistake comes back to the prompt, to be corrected
                return { prompt: prompt(), fill: line };
            }
            return { prompt: prompt() };
        }
        const r = session.eval(text);
        if (r.unfinished)
            return { prompt: prompt() };
        gathered = [];
        trialled = false;
        if (r.ok) {
            remember(text);
            showStack(r.stack);
            const head = /^\s*word\s+([A-Za-z_][A-Za-z_0-9]*)\s*(\([^)]*\))?/.exec(text);
            if (head)
                print("info", `word ${head[1]} ${head[2] ?? "()"} compiled; nothing ran. It runs when it is called: ` +
                    "its arguments first, then its name\n");
            const call = head && sampleCall(head[1], head[2]);
            if (call) {
                print("info", "(a call to try is in the prompt: change its values, Enter runs it)\n");
                return { prompt: prompt(), fill: call };
            }
        }
        return { prompt: prompt() };
    }

    // Inside a word, at each "," or ".": the lines so far, with the blocks still open closed by `end`,
    // are compiled as the word, so a mistake is told at its line. Nothing in the word runs: a definition
    // only declares. A word cannot be checked while it holds the stack apart from its signature's
    // results, so that one message is not a mistake here.
    function checkWord(text) {
        const b = pinax.blocks(text);
        if (!b.ok || !b.open.length || !b.open[0].startsWith("word "))
            return;
        trialLog = [];
        const r = session.eval(text + "\n" + "end\n".repeat(b.open.length));
        const said = trialLog.join("");
        trialLog = null;
        if (r.ok) {
            trialled = true;
            print("info", "(so far it compiles; it is not finished)\n");
        } else if (!/exactly the results of the signature/.test(said)) {
            print("err", placed(said));
            print("info", "(that line is back in the prompt to be corrected; the lines before it stay)\n");
            return false;
        } else {
            print("info", "(so far no mistake; the word is not finished)\n");
        }
    }

    // what the terminal waits for, for the page's status line; pending: the text in the prompt
    function status(pending = "") {
        if (mode?.kind === "edit")
            return `editing line ${mode.at + 1}: Enter puts it back (several lines may take its place), Esc leaves it`;
        if (mode?.kind === "insert")
            return `inserting ${where(mode.first)}, as line ${mode.at + 1}: an empty line ends, and the program runs`;
        const text = [...gathered, pending].join("\n");
        if (text.trim() !== "" && !text.trimStart().startsWith(":")) {
            const b = pinax.blocks(text);
            if (!b.ok)
                return "inside a string: it runs when the string is closed";
            if (b.depth > 0)
                return `in ${plural(b.depth, "block")}, depth ${b.depth}: ${b.open.join(" \u203a ")}. It runs whole at its end`;
            if (gathered.length)
                return "waiting for , or . (or an empty line) to run";
        }
        return `ready \u00b7 program ${plural(lines.length, "line")} \u00b7 stack ${plural(session.stack().length, "value")}`;
    }

    // the page's editor or a loaded file: the whole program, run again
    function setProgram(text) {
        lines = text.replace(/\r\n?/g, "\n").replace(/\s+$/, "").split("\n");
        if (lines.length === 1 && lines[0] === "")
            lines = [];
        keep();
        print("info", `the program has ${plural(lines.length, "line")}; it runs from a fresh state\n`);
        runProgram();
    }

    // Esc: the lines gathered are dropped, an :edit leaves its line, an :insert ends
    function cancel() {
        gathered = [];
        if (trialled) { // a half-written word was compiled in the session: start again from the program
            trialled = false;
            runProgram();
        }
        if (mode?.kind === "insert")
            endInsert();
        mode = null;
        return { prompt: prompt() };
    }

    const saved = store && store.get();
    if (saved) {
        lines = saved.split("\n");
        print("info", `the program of the last visit: ${plural(lines.length, "line")} (:list)\n`);
        runProgram();
    }
    return { enter, cancel, setProgram, status, history, prompt, mode: () => mode?.kind ?? null, program: () => lines.slice(), words: () => userWords(lines.join("\n")), used: () => usedModules(lines.join("\n")) };
}

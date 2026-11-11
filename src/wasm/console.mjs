// console.mjs - the terminal of the Pinax playground, without the page: a session, and a program
// kept beside it as GW-BASIC keeps one, without line numbers. The page (playground.html) draws it.
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
// A line that begins with ":" is a command. Other lines gather until the text ends with "," or
// "."; then it runs as one line of the session. When it ends inside a definition or a block more
// lines gather. A statement that succeeds and begins with a declaration (word, const, global,
// use, set_order and the others) is kept in the program as one entry; one that declares a name
// already kept replaces that entry. enter() gives { prompt }, and for some commands what the page
// is to do: { fill } (text for the prompt), { editor } (the program to edit whole), { download },
// { upload }. MIT (see ../../LICENSE).

const DECLARATIONS = new Set(["word", "const", "global", "array", "list", "pool", "queue", "buffer", "tasks",
    "record", "type", "extern", "forward", "index", "bound", "shared", "test", "use", "set_order"]);
const PROMPT = "pinax> ", MORE = "  ...> ";
const HELP = `Pinax code runs when it ends with "," or "."; a definition may take several lines.
A statement that declares something (word, const, global, use, set_order, ...) is kept in the program.
  :list          the program, entry by entry
  :edit N        entry N into the prompt; entering it replaces N and runs the program again
  :edit          the whole program in an editor (F2; Ctrl+S saves and runs it, Esc leaves)
  :delete N      removes entry N and runs the program again
  :run           a fresh state, then the program from its first entry (F5): the code stays
  :new           no program and a fresh state
  :clear         empties the stack
  :save, :load   the program as a .pnx file
Keys: Up and Down walk the history, Esc drops the lines gathered, Ctrl+L clears the screen.`;

// the declaration a text begins with, as "word sq", "use io"; "" for set_order; null for none
function declared(text) {
    const words = text.replace(/^(\s*rem\b[^\n]*\n)+/, "").trim().split(/[\s(]+/);
    let k = 0;
    if (words[k] === "export" || words[k] === "redefine")
        ++k;
    if (!DECLARATIONS.has(words[k]))
        return null;
    return words[k] === "set_order" ? "" : `${words[k]} ${words[k + 1] ?? ""}`;
}

// the program as entries: a new one at each line that begins, at its first column, with a declaration
export function split(text) {
    const entries = [];
    for (const line of text.replace(/\r\n?/g, "\n").split("\n")) {
        if (entries.length === 0 || (/^[a-z]/.test(line) && declared(line) !== null))
            entries.push(line);
        else
            entries[entries.length - 1] += "\n" + line;
    }
    return entries.map((e) => e.replace(/\s+$/, "")).filter((e) => e !== "");
}

export async function createConsole({ createPinax, bytes, print, store = null }) {
    const pinax = await createPinax(bytes, { write: (stream, text) => print(stream === 1 ? "out" : "err", text) });
    const session = pinax.session("input");
    let program = [];    // the entries, as typed
    let gathered = [];   // the lines of the text not yet run
    let editing = -1;    // the entry the prompt is editing (:edit N), or -1
    const history = [];

    const keep = () => store && store.set(program.join("\n"));
    const showStack = (stack) => stack.length && print("stack", stack.join(" ") + "\n");
    const number = (arg) => {
        const n = Number(arg);
        return Number.isInteger(n) && n >= 1 && n <= program.length ? n - 1 : -1;
    };
    const ends = (text) => /[,.]\s*$/.test(text);

    // a fresh state, then every entry in order; it stops at the first that fails
    function runProgram() {
        session.reset();
        for (let k = 0; k < program.length; ++k) {
            const r = session.eval(program[k]);
            if (!r.ok) {
                print("info", r.unfinished ? `entry ${k + 1} ends inside a definition or a block\n`
                                           : `entry ${k + 1} failed: the program stops there (:edit ${k + 1})\n`);
                return false;
            }
        }
        showStack(session.stack());
        return true;
    }

    function remember(text) {
        const key = declared(text);
        if (key === null)
            return;
        const at = key === "" ? -1 : program.findIndex((e) => declared(e) === key);
        if (at >= 0)
            program[at] = text;
        else
            program.push(text);
        keep();
    }

    function command(line) {
        const [name, arg] = line.slice(1).trim().split(/\s+/, 2);
        switch (name) {
        case "help":
            print("info", HELP + "\n");
            return {};
        case "list":
            if (program.length === 0)
                print("info", "the program is empty\n");
            program.forEach((e, k) => print("info", `${String(k + 1).padStart(3)}  ${e.replace(/\n/g, "\n     ")}\n`));
            return {};
        case "edit": {
            if (arg === undefined)
                return { editor: program.join("\n") };
            const k = number(arg);
            if (k < 0) {
                print("err", `no entry ${arg}; :list shows them\n`);
                return {};
            }
            editing = k;
            return { fill: program[k] };
        }
        case "delete": {
            const k = number(arg);
            if (k < 0) {
                print("err", `no entry ${arg ?? ""}; :list shows them\n`);
                return {};
            }
            program.splice(k, 1);
            keep();
            print("info", `entry ${k + 1} removed\n`);
            runProgram();
            return {};
        }
        case "run":
            runProgram();
            return {};
        case "new":
            program = [];
            keep();
            session.reset();
            print("info", "no program; a fresh state\n");
            return {};
        case "clear":
            session.clear();
            print("info", "the stack is empty\n");
            return {};
        case "save":
            return { download: program.join("\n") + "\n" };
        case "load":
            return { upload: true };
        default:
            print("err", `no command :${name ?? ""}; :help lists them\n`);
            return {};
        }
    }

    function enter(line) {
        print("echo", (gathered.length ? MORE : PROMPT) + line + "\n");
        if (gathered.length === 0 && line.trim() === "")
            return { prompt: PROMPT };
        if (line.trim() !== "")
            history.push(line);
        if (gathered.length === 0 && line.trimStart().startsWith(":")) {
            const r = command(line.trim());
            return { prompt: PROMPT, ...r };
        }
        gathered.push(line);
        const text = gathered.join("\n");
        if (!ends(text))
            return { prompt: MORE };
        if (editing >= 0) { // :edit N: the text takes the entry's place, and the program runs again
            program[editing] = text;
            editing = -1;
            gathered = [];
            keep();
            runProgram();
            return { prompt: PROMPT };
        }
        const r = session.eval(text);
        if (r.unfinished)
            return { prompt: MORE };
        gathered = [];
        if (r.ok) {
            remember(text);
            showStack(r.stack);
        }
        return { prompt: PROMPT };
    }

    // the page's editor or a loaded file: the whole program, split into entries, run again
    function setProgram(text) {
        program = split(text);
        keep();
        print("info", `the program has ${program.length} entr${program.length === 1 ? "y" : "ies"}\n`);
        runProgram();
    }

    // Esc: the lines gathered, and an :edit in progress, are dropped
    function cancel() {
        gathered = [];
        editing = -1;
        return { prompt: PROMPT };
    }

    const saved = store && store.get();
    if (saved) {
        program = split(saved);
        print("info", `the program of the last visit: ${program.length} entr${program.length === 1 ? "y" : "ies"} (:list)\n`);
        runProgram();
    }
    return { enter, cancel, setProgram, history, program: () => program.slice(), prompt: () => (gathered.length ? MORE : PROMPT) };
}

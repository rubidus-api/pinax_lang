// pinax.mjs - runs Pinax in a browser or in node, on the module ./nob wasm builds.
//
//   import { createPinax } from "./pinax.mjs";
//   const pinax = await createPinax(wasmBytes, { write: (stream, text) => ... });
//   const { ok, error } = pinax.run('use io\n"hello" io.println\n', "hello");
//
//   const s = pinax.session();
//   s.eval("3 4");            // { ok: true, unfinished: false, stack: ["3", "4"] }
//   s.eval("add.");           // { ok: true, unfinished: false, stack: ["7"] }
//   s.eval("word f (gives int)");   // { ok: false, unfinished: true, ... }: give it more text
//
// write(stream, text) receives what io.print and io.println print (stream 1) and io.eprint and
// what goes wrong (stream 2), as pnx writes them: a load error with its place, a fault with its
// line and the locals of the word it happened in. run() opens a fresh state, loads the source
// under `name`, runs the code outside definitions and returns { ok: true } or
// { ok: false, error: "what went wrong" }. io.read_line always fails with not_found.
//
// session() gives a session as `pnx repl` has (pinax_eval): eval(text) runs the text as one line
// of it; what earlier texts defined and left stays. It returns { ok, error, unfinished, stack },
// `stack` the values left, the deepest first, written as pnx repl writes them. An unfinished
// text (it ends inside a definition or a block) writes nothing and changes nothing: give it again
// with more. Tool output (`get_order`, `stack`) is written on stream 1. reset() starts the
// session again from nothing; clear() empties its stack. The module holds one state: run()
// ends a session, which starts again from nothing at its next eval().
// MIT (see ../../LICENSE).

const SYNTAX = 0x202, TYPE = 0x203;
const errors = new Map([
    [0x100, "failed"], [0x101, "integer overflow"], [0x102, "division by zero"], [0x103, "index out of range"],
    [0x104, "a declared capacity is used up"], [0x105, "call depth limit"],
    [0x106, "a row or handle was used after it was given back"],
    [0x109, "deadlock: waiting for a queue that nothing can change"], [0x201, "the memory block is too small"],
    [0x204, "not found"], [0x206, "bad format"],
]);
const VALUE_BOOL = 2, VALUE_STR = 3, VALUE_ERR = 4;

export async function createPinax(bytes, { write = () => {}, blockBytes = 16 << 20 } = {}) {
    const decoder = new TextDecoder();
    const encoder = new TextEncoder();
    let memory = null;
    let file = "";
    let told = false;       // a load message named the place: the failure is not said again
    let held = null;        // a session line's output, kept until the line is known to be whole
    let toolStream = 2;     // where tool output (line 0) goes: stream 1 in a session
    let opened = 0;         // how many times a state was opened: a session sees when run() took it
    const out = (stream, text) => (held ? held.push([stream, text]) : write(stream, text));
    const view = (at, length) => new Uint8Array(memory.buffer, at, length);
    const imports = {
        env: {
            pnx_write: (stream, at, length) => out(stream, decoder.decode(view(at, length))),
            pnx_diag: (line, column, at, length) => {
                const message = decoder.decode(view(at, length));
                told = told || line !== 0;
                if (line === 0)
                    out(toolStream, message + "\n");
                else
                    out(2, `${file}:${line}:${column}: ${message}\n`);
            },
        },
    };
    const { instance } = await WebAssembly.instantiate(bytes, imports);
    const x = instance.exports;
    memory = x.memory;

    const open = () => (++opened, x.pnx_wasm_open(blockBytes));
    // the text at a place of the module's memory, with `extra` bytes after it; 0 when there is no room
    function place(text, extra = new Uint8Array(0)) {
        const at = x.pnx_wasm_text(text.length + extra.length + 1);
        if (at) {
            view(at, text.length).set(text);
            view(at + text.length, extra.length).set(extra);
        }
        return at;
    }
    const errorText = () => decoder.decode(view(x.pnx_wasm_error(), x.pnx_wasm_error_length()));
    // a value packed as pnx_wasm_fault_local and pnx_wasm_eval_value give it
    function shown(at, short) {
        const w = new Uint32Array(memory.buffer, at, 9);
        const text = (i) => decoder.decode(view(w[i], w[i + 1]));
        const i = BigInt.asIntN(64, (BigInt(w[2]) << 32n) | BigInt(w[1]));
        if (w[0] === VALUE_STR) {
            const s = text(7);
            return short && s.length > 60 ? `"${s.slice(0, 60)}..."` : `"${s}"`;
        }
        return w[0] === VALUE_ERR ? `err.${text(7)}` : w[0] === VALUE_BOOL ? (i ? "true" : "false") : `${i}`;
    }
    // what went wrong, as pnx says it; the locals of the word a fault happened in
    function report(err, name) {
        const kind = errors.get(err) ?? "error";
        if (!told && err !== SYNTAX && err !== TYPE) { // those two are always reported with their place
            // a fault knows its line: "line 12: ..." or "module:12: ..."
            const said = errorText();
            const line = err >= 0x100 && err < 0x200 ? /^(?:line |[^:]*:)(\d+):/.exec(said) : null;
            out(2, line ? `${name}:${line[1]}: ${kind}\n` : `${name}: ${kind}\n`);
        }
        for (let k = 0; ; ++k) {
            const at = x.pnx_wasm_fault_local(k);
            if (!at)
                break;
            const w = new Uint32Array(memory.buffer, at, 9);
            const text = (i) => decoder.decode(view(w[i], w[i + 1]));
            out(2, `  ${text(3)} ${text(5)} = ${shown(at, false)}\n`);
        }
        return kind;
    }

    function run(source, name = "main") {
        file = name;
        told = false;
        toolStream = 2;
        let err = open();
        if (err === 0) {
            const text = encoder.encode(source);
            const at = place(text, encoder.encode(name + "\0"));
            err = at ? x.pnx_wasm_load(at, text.length, at + text.length) : 0x201;
        }
        if (err === 0)
            return { ok: true };
        return { ok: false, error: report(err, name) };
    }

    function session(name = "input") {
        let mine = -1; // the opening this session's state came from
        const stack = () => {
            if (mine !== opened)
                return [];
            const values = [];
            for (let k = 0, n = x.pnx_wasm_eval_count(); k < n; ++k)
                values.push(shown(x.pnx_wasm_eval_value(k), true));
            return values;
        };
        function reset() {
            const err = open();
            mine = opened;
            return err === 0;
        }
        function evaluate(source) {
            if (mine !== opened && !reset())
                return { ok: false, error: errors.get(0x201), unfinished: false, stack: [] };
            file = name;
            told = false;
            toolStream = 1;
            held = [];
            const text = encoder.encode(source);
            const at = place(text);
            const err = at ? x.pnx_wasm_eval(at, text.length) : 0x201;
            const lines = held;
            held = null;
            if (err === SYNTAX && errorText().startsWith("unfinished:"))
                return { ok: false, error: "unfinished", unfinished: true, stack: stack() };
            for (const [stream, t] of lines)
                write(stream, t);
            const error = err === 0 ? undefined : report(err, name);
            return { ok: err === 0, error, unfinished: false, stack: stack() };
        }
        return { eval: evaluate, reset, clear: () => mine === opened && x.pnx_wasm_eval_clear(), stack };
    }

    return { run, session, close: () => x.pnx_wasm_close() };
}

// pinax.mjs - runs Pinax in a browser or in node, on the module ./nob wasm builds.
//
//   import { createPinax } from "./pinax.mjs";
//   const pinax = await createPinax(wasmBytes, { write: (stream, text) => ... });
//   const { ok, error } = pinax.run('use io\n"hello" io.println\n', "hello");
//
// write(stream, text) receives what io.print and io.println print (stream 1) and io.eprint and
// what goes wrong (stream 2), as pnx writes them: a load error with its place, a fault with its
// line and the locals of the word it happened in. run() opens a fresh state, loads the source
// under `name`, runs the code outside definitions and returns { ok: true } or
// { ok: false, error: "what went wrong" }. io.read_line always fails with not_found.
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
    let told = false; // a load message named the place: the failure is not said again
    const view = (at, length) => new Uint8Array(memory.buffer, at, length);
    const imports = {
        env: {
            pnx_write: (stream, at, length) => write(stream, decoder.decode(view(at, length))),
            pnx_diag: (line, column, at, length) => {
                const message = decoder.decode(view(at, length));
                told = told || line !== 0;
                write(2, line === 0 ? message + "\n" : `${file}:${line}:${column}: ${message}\n`);
            },
        },
    };
    const { instance } = await WebAssembly.instantiate(bytes, imports);
    const x = instance.exports;
    memory = x.memory;

    function run(source, name = "main") {
        file = name;
        told = false;
        let err = x.pnx_wasm_open(blockBytes);
        if (err === 0) {
            const text = encoder.encode(source);
            const label = encoder.encode(name + "\0");
            const at = x.pnx_wasm_text(text.length + label.length);
            if (!at) {
                err = 0x201;
            } else {
                view(at, text.length).set(text);
                view(at + text.length, label.length).set(label);
                err = x.pnx_wasm_load(at, text.length, at + text.length);
            }
        }
        if (err === 0)
            return { ok: true };
        const kind = errors.get(err) ?? "error";
        if (!told && err !== SYNTAX && err !== TYPE) { // those two are always reported with their place
            // a fault knows its line: "line 12: ..." or "module:12: ..."
            const said = decoder.decode(view(x.pnx_wasm_error(), x.pnx_wasm_error_length()));
            const line = err >= 0x100 && err < 0x200 ? /^(?:line |[^:]*:)(\d+):/.exec(said) : null;
            write(2, line ? `${name}:${line[1]}: ${kind}\n` : `${name}: ${kind}\n`);
        }
        for (let k = 0; ; ++k) { // the locals of the word the fault happened in
            const at = x.pnx_wasm_fault_local(k);
            if (!at)
                break;
            const w = new Uint32Array(memory.buffer, at, 9);
            const text = (i) => decoder.decode(view(w[i], w[i + 1]));
            const i = BigInt.asIntN(64, (BigInt(w[2]) << 32n) | BigInt(w[1]));
            const value = w[0] === VALUE_STR ? `"${text(7)}"` : w[0] === VALUE_ERR ? `err.${text(7)}`
                        : w[0] === VALUE_BOOL ? (i ? "true" : "false") : `${i}`;
            write(2, `  ${text(3)} ${text(5)} = ${value}\n`);
        }
        return { ok: false, error: kind };
    }

    return { run, close: () => x.pnx_wasm_close() };
}

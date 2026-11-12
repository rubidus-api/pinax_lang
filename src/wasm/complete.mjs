// complete.mjs - completion for the playground's prompt: which words fit what is being typed.
//
//   import { complete } from "./complete.mjs";
//   import { WORDS } from "./words.mjs";            // generated from the standard (docs/tools/words.py)
//   complete(WORDS, "\"a\" str.tr", 10);
//   // { from: 4, to: 10, items: [{ label: "str.trim", insert: "str.trim", module: "str",
//   //     word: "trim", signature: "(str gives str)", meaning: "..." }, ...] }
//
// The word being typed is the run of letters, digits, "_" and "." that ends at the cursor. With a
// "." in it, the part before is a module and only its words are offered; otherwise the words of the
// core and of every module whose name begins so, the core first. A word of a module is inserted
// with its module ("str.trim"), which is right whatever set_order says. A word of the program
// (a fifth member, true) comes first. null when nothing fits.
//
// Also: `:` at the start of a line lists the terminal's commands (commandsOf(GUIDE) makes the list); nearby() gives, for
// an empty place (Alt+0), up to ten names the program declares (words, variables, parameters), the nearest to the
// cursor and the most recently written first.
// MIT (see ../../LICENSE).

// After `use ` or inside the parentheses of `set_order`: { stem, taken } (the part of the name typed, and the modules
// the list names already), else null. There the names offered are the modules, not words.
export function moduleContext(before) {
    const use = /(?:^|\n)[ \t]*use[ \t]+([A-Za-z_0-9]*)$/.exec(before);
    if (use)
        return { kind: "use", stem: use[1], taken: [] };
    const order = /(?:^|[\s,.])set_order[ \t]*\(([^)]*?)([A-Za-z_0-9]*)$/.exec(before);
    if (order && /(^|,)\s*$/.test(order[1]))
        return { kind: "order", stem: order[2], taken: order[1].split(",").map((t) => t.trim()).filter(Boolean) };
    return null;
}

// the commands of the terminal, from its GUIDE: [{ name: ":list", usage: ":list [N[-M]]", what }]
export function commandsOf(guide) {
    const out = new Map();
    for (const [usage, what] of guide.commands)
        for (const name of usage.match(/:[a-z_]+/g) ?? [])
            if (!out.has(name))
                out.set(name, { name, usage, what });
    return [...out.values()];
}
// at the start of a line, `:` and the letters after it (the command name being typed): the stem, else null
export function commandContext(before) {
    const m = /(?:^|\n)[ \t]*:([a-z_]*)$/.exec(before);
    return m ? { stem: m[1] } : null;
}

// the names a text declares: Map name -> { kind, signature }; the parameters of a word count as variables
export function declaredNames(text) {
    const out = new Map();
    for (const m of text.matchAll(/^[ \t]*(?:export[ \t]+|redefine[ \t]+)?(word|const|global|array|list|pool|queue|buffer|tasks|record|type|index|bound|shared)[ \t]+([A-Za-z_][A-Za-z_0-9]*)[ \t]*(\([^)]*\))?/gm)) {
        out.set(m[2], { kind: m[1] === "word" ? "word" : m[1] === "const" ? "constant" : "variable", signature: m[1] === "word" ? m[3] ?? "()" : "" });
        if (m[1] === "word" && m[3])
            for (const param of m[3].slice(1, -1).split("gives")[0].split(","))
                if (/^\s*([A-Za-z_][A-Za-z_0-9]*)\s+\S/.test(param)) {
                    const name = /^\s*([A-Za-z_][A-Za-z_0-9]*)/.exec(param)[1];
                    if (!out.has(name))
                        out.set(name, { kind: `parameter of ${m[2]}`, signature: "" });
                }
    }
    return out;
}

// for an empty place: the names the program declares, nearest to the cursor and most recently written first
// (what stands before the cursor, backwards; then `context`, an older text, backwards; then what follows the cursor),
// ten at most. A name met nowhere comes last, in the order declared.
export function nearby(text, cursor, context = "", limit = 10) {
    const declared = declaredNames(context + "\n" + text);
    if (!declared.size)
        return null;
    const names = (t) => [...t.matchAll(/[A-Za-z_][A-Za-z_0-9]*/g)].map((m) => m[0]);
    const order = [...names(text.slice(0, cursor)).reverse(), ...names(context).reverse(), ...names(text.slice(cursor))];
    const seen = new Set();
    const items = [];
    for (const name of [...order, ...declared.keys()]) {
        if (seen.has(name) || !declared.has(name))
            continue;
        seen.add(name);
        const { kind, signature } = declared.get(name);
        items.push({ label: name, insert: name, module: "", word: name, signature, meaning: kind });
        if (items.length === limit)
            break;
    }
    return { from: cursor, to: cursor, items };
}

export function complete(words, text, cursor, limit = 8, modules = [], used = [], commands = []) {
    const before = text.slice(0, cursor);
    const line = before.slice(before.lastIndexOf("\n") + 1);
    if (commands.length && /^[ \t]*:/.test(line)) { // a command line: the name, and nothing after it
        const c = commandContext(before);
        const items = c ? commands.filter((k) => k.name.slice(1).startsWith(c.stem)).map((k) => ({
            label: k.name, insert: k.name, module: "", word: "", signature: k.usage === k.name ? "" : k.usage, meaning: k.what })) : [];
        return items.length && !(items.length === 1 && items[0].insert === ":" + c.stem)
            ? { from: cursor - c.stem.length - 1, to: cursor, items } : null;
    }
    const where = moduleContext(before);
    if (where) {
        // `use` offers the modules not in use yet; set_order offers the modules in use, and get_order for the order as it is
        const pool = where.kind === "use" ? modules.filter(([m]) => !used.includes(m))
            : modules.filter(([m]) => used.includes(m));
        const items = pool.filter(([m]) => m.startsWith(where.stem) && !where.taken.includes(m)).map(([m, what]) => {
            const names = words.filter((w) => w[0] === m).map((w) => w[1]);
            const list = names.length ? `; words: ${names.slice(0, 8).join(", ")}${names.length > 8 ? ", ..." : ""}` : "";
            return { label: m, insert: m, module: m, word: "", signature: "", meaning: `${what}${list}` };
        });
        if (where.kind === "order" && "get_order".startsWith(where.stem) && !where.taken.includes("get_order"))
            items.push({ label: "get_order", insert: "get_order", module: "", word: "get_order", signature: "",
                         meaning: "the order as it is now: set_order (get_order, str) puts str behind it" });
        return items.length && !(items.length === 1 && items[0].insert === where.stem)
            ? { from: cursor - where.stem.length, to: cursor, items: items.slice(0, 12) } : null;
    }
    const typed = /[A-Za-z_][A-Za-z_0-9.]*$/.exec(before)?.[0];
    if (!typed || /^[0-9]/.test(typed))
        return null;
    const dot = typed.lastIndexOf(".");
    const module = dot >= 0 ? typed.slice(0, dot) : null;
    const stem = dot >= 0 ? typed.slice(dot + 1) : typed;
    const items = [];
    for (const [mod, word, signature, meaning, mine] of words) {
        if (module !== null ? mod !== module : !word.startsWith(stem))
            continue;
        if (module !== null && !word.startsWith(stem))
            continue;
        const label = mod === "" ? word : `${mod}.${word}`;
        items.push({ label, insert: label, module: mod, word, signature, meaning,
                     rank: (mine ? -2 : 0) + (word === stem ? 0 : 2) + (mod === "" ? 0 : 1) });
    }
    if (module === null)
        for (const mod of new Set(words.map((w) => w[0])))
            if (mod !== "" && mod.startsWith(stem) && mod.length > stem.length)
                items.push({ label: `${mod}.`, insert: `${mod}.`, module: mod, word: "", signature: "",
                             meaning: `the module ${mod}: its words follow the dot`, rank: 4 });
    items.sort((a, b) => a.rank - b.rank || a.label.localeCompare(b.label));
    if (!items.length || (items.length === 1 && items[0].insert === typed && !items[0].signature))
        return null;
    return { from: cursor - typed.length, to: cursor, items: items.slice(0, limit).map(({ rank, ...i }) => i) };
}

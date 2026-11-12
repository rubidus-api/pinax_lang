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
// MIT (see ../../LICENSE).

// After `use ` or inside the parentheses of `set_order`: { stem, taken } (the part of the name typed, and the modules
// the list names already), else null. There the names offered are the modules, not words.
export function moduleContext(before) {
    const use = /(?:^|\n)[ \t]*use[ \t]+([A-Za-z_0-9]*)$/.exec(before);
    if (use)
        return { stem: use[1], taken: [] };
    const order = /(?:^|[\s,.])set_order[ \t]*\(([^)]*?)([A-Za-z_0-9]*)$/.exec(before);
    if (order && /(^|,)\s*$/.test(order[1]))
        return { stem: order[2], taken: order[1].split(",").map((t) => t.trim()).filter(Boolean) };
    return null;
}

export function complete(words, text, cursor, limit = 8, modules = []) {
    const before = text.slice(0, cursor);
    const where = moduleContext(before);
    if (where) {
        const items = modules.filter(([m]) => m.startsWith(where.stem) && !where.taken.includes(m)).map(([m, what]) => {
            const names = words.filter((w) => w[0] === m).map((w) => w[1]);
            const list = names.length ? `; words: ${names.slice(0, 8).join(", ")}${names.length > 8 ? ", ..." : ""}` : "";
            return { label: m, insert: m, module: m, word: "", signature: "", meaning: `${what}${list}` };
        });
        return items.length && !(items.length === 1 && items[0].insert === where.stem) ? { from: cursor - where.stem.length, to: cursor, items: items.slice(0, 12) } : null;
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

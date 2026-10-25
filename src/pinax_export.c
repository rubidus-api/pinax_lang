// pinax_export.c - a program's code written out as C (RFC-0028, D0064).
//
// The whole code of the state becomes one C function with a label for every instruction. Each
// instruction the interpreter runs as one call of its pnx_op_ or pnx_cond_ function (PNX_RUN_KINDS)
// is that call with the instruction's word as a constant; a jump is a goto; CHECK, FAULT, CALL, TCALL
// and RET are written with the functions pinax_exec.h uses for them (pnx_enter_word, pnx_tail_word,
// pnx_exec_end, pnx_exec_suspend). Every other instruction is given to the interpreter itself, one at
// a time (pnx_step), and the function goes on from wherever that left the run: so tasks, host words,
// waiting and the rest are the interpreter's own. The function works on the state as the interpreter
// does, and is entered through a switch on the pc, so a run can stop and go on anywhere.
//
// The output is plain text; no C library is used here (the core is freestanding).

#include "pinax_ops.h"

typedef struct out {
    char  *p;
    size_t len, cap;
} out;

static void put(out *o, const char *s) {
    for (; *s; ++s) {
        if (o->len < o->cap)
            o->p[o->len] = *s;
        ++o->len;
    }
}

static void put_u(out *o, uint64_t v) {
    char d[24];
    int  n = 0;
    do
        d[n++] = (char)('0' + v % 10);
    while ((v /= 10) != 0);
    char s[24];
    for (int k = 0; k < n; ++k)
        s[k] = d[n - 1 - k];
    s[n] = 0;
    put(o, s);
}

static void put_hex(out *o, uint64_t v, int digits) {
    char s[24] = "0x";
    for (int k = 0; k < digits; ++k)
        s[2 + k] = "0123456789abcdef"[(v >> (4 * (digits - 1 - k))) & 15u];
    s[2 + digits] = 0;
    put(o, s);
}

enum { K_NONE, K_RUN, K_RUN2, K_RUNG, K_RUNGW, K_RUNF, K_RUNFG, K_RUNFGW, K_RUNF2, K_RUNJ };

static const char *const op_name[PNX_OP_COUNT] = {
#define PNX_NAME(name, number, a, b, c, shape, second, fails, roles) [PNX_##name] = #name,
    PNX_OPS(PNX_NAME)
#undef PNX_NAME
};

// The narrow and the wide form of RUNG and RUNFG share one function, named by the narrow form.
#define K_RUN_(name) [PNX_##name] = K_RUN,
#define K_RUN2_(name) [PNX_##name] = K_RUN2,
#define K_RUNG_(name) [PNX_##name] = K_RUNG, [PNX_##name##W] = K_RUNGW,
#define K_RUNF_(name) [PNX_##name] = K_RUNF,
#define K_RUNFG_(name) [PNX_##name] = K_RUNFG, [PNX_##name##W] = K_RUNFGW,
#define K_RUNF2_(name) [PNX_##name] = K_RUNF2,
#define K_RUNJ_(name) [PNX_##name] = K_RUNJ,
#define PNX_KIND(kind, name) K_##kind##_(name)
static const uint8_t op_kind[PNX_OP_COUNT] = {PNX_RUN_KINDS(PNX_KIND)};
#undef PNX_KIND
#undef K_RUN_
#undef K_RUN2_
#undef K_RUNG_
#undef K_RUNF_
#undef K_RUNFG_
#undef K_RUNF2_
#undef K_RUNJ_

// The function of a wide form is the narrow form's: AGETW calls pnx_op_AGET with x the second word.
static void put_fn(out *o, const char *prefix, uint32_t op, bool wide) {
    put(o, prefix);
    const char *name = op_name[op];
    size_t      n = 0;
    while (name[n])
        ++n;
    char buf[32];
    size_t k = 0;
    for (; k < n - (wide ? 1 : 0) && k + 1 < sizeof buf; ++k)
        buf[k] = name[k];
    buf[k] = 0;
    put(o, buf);
}

static void label(out *o, uint32_t at) {
    put(o, "L");
    put_u(o, at);
}

static void go_to(out *o, uint32_t at) {
    put(o, "goto ");
    label(o, at);
    put(o, ";");
}

// "{ next = AT; goto fault; }": the run ends with the fault in r, the pc past the instruction.
static void on_fault(out *o, uint32_t next) {
    put(o, "{ next = ");
    put_u(o, next);
    put(o, "u; goto fault; }");
}

static void call_op(out *o, uint32_t op, bool wide, bool proven, uint32_t w, const char *x) {
    put_fn(o, "pnx_op_", op, wide);
    if (proven) // RFC-0028 P3: the loader proved its check cannot fire
        put(o, "_unchecked");
    put(o, "(st, R, S, ");
    put_hex(o, w, 8);
    put(o, "u, ");
    put(o, x);
    put(o, ")");
}

size_t pnx_export_c(const pinax_state *st, const char *name, char *buffer, size_t cap) {
    out o = {buffer, 0, cap};
    const uint32_t n = st->head ? st->head->n_code : 0;
    const uint32_t *const code = st->code;
    put(&o, "/* Written by pnx export-c (RFC-0028): the code of one program as C. Do not edit; export again\n"
            " * after the program changes. Compile with the build that wrote it and attach the table below with\n"
            " * pinax_native_attach(). */\n#include \"pinax_native.h\"\n\n");
    put(&o, "// The run's place into the state, one instruction by the interpreter, and the place back out of it.\n"
            "#define PNX_STEP_OUT(step)                                                                     \\\n"
            "    do {                                                                                       \\\n"
            "        st->pc = at; st->int_base = ibase; st->str_base = sbase; st->depth = depth;              \\\n"
            "        st->depth_floor = floor; st->left = left; st->instructions += count; count = 0;          \\\n"
            "        const pinax_err e = step(st);                                                            \\\n"
            "        if (e != PNX_STEP_ON)                                                                    \\\n"
            "            return e;                                                                            \\\n"
            "        at = st->pc; ibase = st->int_base; sbase = st->str_base; depth = st->depth;              \\\n"
            "        floor = st->depth_floor; left = st->left; R = st->ibank + ibase; S = st->sbank + sbase;  \\\n"
            "    } while (0)\n\n");
    put(&o, "static pinax_err run_");
    put(&o, name);
    put(&o, "(pinax_state *st) {\n"
            "    uint32_t ibase = st->int_base, sbase = st->str_base, depth = st->depth, floor = st->depth_floor;\n"
            "    int64_t *R = st->ibank + ibase;\n"
            "    pnx_scell *S = st->sbank + sbase;\n"
            "    const uint64_t budget = st->counted;\n"
            "    uint64_t left = st->left, count = 0;\n"
            "    uint32_t at = st->pc, next = 0;\n"
            "    int r = PNX_R_OK;\n"
            "    (void)budget;\n"
            "    if (at == UINT32_MAX) { // never: the two ways out are named even when no instruction needs them\n"
            "        if (next) goto fault;\n"
            "        goto suspend;\n"
            "    }\n"
            "go:\n"
            "    switch (at) {\n");
    for (uint32_t i = 0; i < n; i += pnx_ops_info[code[i] & 0xffu].second ? 2u : 1u) {
        put(&o, "    case ");
        put_u(&o, i);
        put(&o, ": ");
        go_to(&o, i);
        put(&o, "\n");
    }
    put(&o, "    default: goto step;\n    }\n");
    for (uint32_t i = 0; i < n;) {
        const uint32_t w = code[i], op = w & 0xffu;
        const pnx_opinfo info = pnx_ops_info[op];
        const uint32_t len = info.second ? 2u : 1u, after = i + len;
        const uint32_t x = info.second && i + 1 < n ? code[i + 1] : 0;
        char second[24];
        { // the second word as a constant
            out t = {second, 0, sizeof second - 1};
            put_hex(&t, x, 8);
            put(&t, "u");
            second[t.len < sizeof second ? t.len : sizeof second - 1] = 0;
        }
        char narrow[24];
        {
            out t = {narrow, 0, sizeof narrow - 1};
            put_u(&t, OP_C(w));
            put(&t, "u");
            narrow[t.len < sizeof narrow ? t.len : sizeof narrow - 1] = 0;
        }
        label(&o, i);
        put(&o, ": ");
        switch (op_kind[op]) {
        case K_RUN:
        case K_RUN2:
        case K_RUNG:
        case K_RUNGW: {
            const bool wide = op_kind[op] == K_RUNGW;
            const bool proven = st->proven && st->proven[i] != PNX_PROVEN_NONE && pnx_checked_op(op);
            put(&o, "if ((r = ");
            call_op(&o, op, wide, proven, w, op_kind[op] == K_RUN ? "0" : op_kind[op] == K_RUNG ? narrow : second);
            put(&o, ") != PNX_R_OK) ");
            on_fault(&o, after);
            break;
        }
        case K_RUNF:
        case K_RUNF2:
        case K_RUNFG:
        case K_RUNFGW: { // success skips the failure word that follows
            const bool wide = op_kind[op] == K_RUNFGW;
            put(&o, "if ((r = ");
            call_op(&o, op, wide, false, w, op_kind[op] == K_RUNF ? "0" : op_kind[op] == K_RUNFG ? narrow : second);
            put(&o, ") == PNX_R_OK) ");
            go_to(&o, after + 1);
            put(&o, " if (r != PNX_R_FAILED) ");
            on_fault(&o, after);
            break;
        }
        case K_RUNJ:
            put_fn(&o, "if (pnx_cond_", op, false);
            put(&o, "(st, R, S, ");
            put_hex(&o, w, 8);
            put(&o, "u)) ");
            go_to(&o, (uint32_t)((int32_t)after + OP_SC(w)));
            break;
        default:
            switch (op) {
            case PNX_JMP:
                go_to(&o, (uint32_t)((int32_t)after + OP_SAX(w)));
                break;
            case PNX_CHECK:
                put(&o, "{ uint64_t c = ");
                put_u(&o, OP_BX(w));
                put(&o, "u; const uint64_t charge = st->charge; if (charge >= 16) c += charge >> 4;\n"
                        "    if (budget) { if (left < c) { st->owed = c > UINT32_MAX ? UINT32_MAX : (uint32_t)c; at = ");
                put_u(&o, i);
                put(&o, "u; goto suspend; } left -= c; }\n    count += c; if (charge >= 16) st->charge = charge & 15u; }");
                break;
            case PNX_FAULT:
                put(&o, "r = ");
                put_u(&o, OP_BX(w));
                put(&o, "; ");
                on_fault(&o, after);
                break;
            case PNX_CALL:
                put(&o, "if ((r = pnx_enter_word(st, &st->words[");
                put_u(&o, x);
                put(&o, "], ");
                put_hex(&o, w, 8);
                put(&o, "u, ");
                put_u(&o, after);
                put(&o, "u, &ibase, &sbase, &depth)) != PNX_R_OK) ");
                on_fault(&o, after);
                put(&o, " R = st->ibank + ibase; S = st->sbank + sbase; ");
                go_to(&o, st->words[x].entry);
                break;
            case PNX_TCALL:
                put(&o, "if ((r = pnx_tail_word(st, &st->words[");
                put_u(&o, x);
                put(&o, "], ibase, sbase)) != PNX_R_OK) ");
                on_fault(&o, after);
                put(&o, " ");
                go_to(&o, st->words[x].entry);
                break;
            case PNX_RET: // to the caller; at the bottom of the run (the end of a task, of the run) the interpreter decides
                put(&o, "if (depth == floor) { at = ");
                put_u(&o, i);
                put(&o, "u; goto step; } { const pnx_frame f = st->ctrl[--depth]; at = f.pc; ibase = f.int_base; "
                        "sbase = f.str_base; R = st->ibank + ibase; S = st->sbank + sbase; goto go; }");
                break;
            default: // the interpreter does it; mostly the run goes on right after it
                put(&o, "at = ");
                put_u(&o, i);
                // a host call is done in place, without the interpreter's dispatch (D0066)
                put(&o, op == PNX_HCALL || op == PNX_HCALLF ? "u; PNX_STEP_OUT(pnx_step_host); if (at == "
                                                            : "u; PNX_STEP_OUT(pnx_step); if (at == ");
                if (op == PNX_HCALLF) { // a good result goes on past the failure word
                    put_u(&o, after + 1);
                    put(&o, "u) ");
                    go_to(&o, after + 1);
                    put(&o, " if (at == ");
                }
                put_u(&o, after);
                put(&o, "u) ");
                go_to(&o, after);
                put(&o, " goto go; /* ");
                put(&o, op_name[op] ? op_name[op] : "?");
                put(&o, " */");
                break;
            }
        }
        put(&o, "\n");
        i = after;
    }
    put(&o, "step: // the interpreter runs this one instruction, and the run goes on from where it stands then\n"
            "    PNX_STEP_OUT(pnx_step);\n"
            "    goto go;\n"
            "fault: // or a task of an `apart` set ends alone, and this is entered again where the run stands\n"
            "    return pnx_exec_fault(st, (pinax_err)r, next, ibase, sbase, R, floor, left, count);\n"
            "suspend: // or a task's turn ran out, and it faults\n"
            "    return pnx_exec_out(st, at, ibase, sbase, depth, count);\n"
            "}\n#undef PNX_STEP_OUT\n\n");
    put(&o, "const pinax_native ");
    put(&o, name);
    put(&o, " = {sizeof(pinax_native), ");
    put_hex(&o, pnx_native_hash(st), 16);
    put(&o, "u, run_");
    put(&o, name);
    put(&o, "};\n");
    if (o.len < o.cap)
        o.p[o.len] = 0;
    return o.len < o.cap ? o.len : 0;
}

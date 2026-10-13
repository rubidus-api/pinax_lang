// pinax_vm.c - the interpreter loop (RFC-0010): fetch, dispatch, calls and returns.
// What each instruction does is in pinax_ops.h (RFC-0013 J2).
//
// Registers are typed by the instruction, so nothing is tagged and nothing is checked at
// run time except what the language defines as a fault: overflow, division by zero, an
// index outside a declared capacity, the depth limit.
//
// Dispatch is computed goto; define PINAX_SWITCH_DISPATCH for a plain switch. The budget is
// charged by CHECK instructions before backward branches and calls; define
// PINAX_BUDGET_EACH to charge every instruction instead.

#include "pinax_ops.h"

#if !defined(PINAX_SWITCH_DISPATCH)
#pragma GCC diagnostic ignored "-Wpedantic" // labels as values
#endif

uint8_t *pnx_region_alloc(pinax_state *st, size_t bytes) {
    if (bytes > st->region_len - st->region_used)
        return NULL;
    uint8_t *p = st->region + st->region_used;
    st->region_used += (uint32_t)bytes;
    return p;
}

// Calls a host word. Arguments start at ia / sa; results follow the arguments. Returns the
// host's outcome; *fault is set when the interpreter itself cannot go on.
static pinax_err host_call(pinax_state *st, const pnx_host *h, int64_t *ia, pnx_scell *sa, bool *fault) {
    pinax_str sv[PNX_MAX_STR_ARGS];
    pinax_str rs[PNX_MAX_STR_RESULTS];
    const pnx_counts n = h->counts;
    for (uint32_t k = 0; k < n.str_args; ++k) {
        sv[k].ptr = pnx_view_bytes(st, sa[k]);
        sv[k].len = sa[k].length;
    }
    for (uint32_t k = 0; k < n.str_results; ++k) {
        rs[k].ptr = NULL;
        rs[k].len = 0;
    }
    pinax_args args = {ia, ia + n.int_args, sv, rs, NULL, NULL};
    pinax_call call = {st};
    pinax_err err = h->fn(&call, h->user, &args);
    *fault = false;
    if (err != PINAX_OK) {
        if (err < PINAX_ERR_FAILED) { // suspending or pending host words are not in P0
            *fault = true;
            return PINAX_ERR_STATE;
        }
        return err;
    }
    for (uint32_t k = 0; k < n.str_results; ++k) {
        pnx_scell *cell = &sa[n.str_args + k];
        if (rs[k].len > UINT32_MAX) {
            *fault = true;
            return PINAX_ERR_RANGE;
        }
        uint32_t len = (uint32_t)rs[k].len;
        if (len == 0) {
            *cell = (pnx_scell){PNX_SEG_EMPTY, 0, 0};
        } else if (rs[k].ptr >= st->region && rs[k].ptr + len <= st->region + st->region_used) {
            *cell = (pnx_scell){PNX_SEG_REGION, len, (uint64_t)(rs[k].ptr - st->region)};
        } else if (h->flags & PINAX_WORD_KEEP_STR) {
            if (PNX_SEG_HOST + st->host_segments >= PNX_SEG_COUNT) {
                *fault = true;
                return PINAX_ERR_FULL;
            }
            uint32_t segment = PNX_SEG_HOST + st->host_segments++;
            st->segment[segment] = rs[k].ptr;
            *cell = (pnx_scell){segment, len, 0};
        } else {
            uint8_t *copy = pnx_region_alloc(st, len);
            if (!copy) {
                *fault = true;
                return PINAX_ERR_FULL;
            }
            __builtin_memcpy(copy, rs[k].ptr, len);
            *cell = (pnx_scell){PNX_SEG_REGION, len, (uint64_t)(copy - st->region)};
        }
    }
    return PINAX_OK;
}

pinax_err pnx_execute(pinax_state *st) {
    const uint32_t *const code = st->code;
    const uint32_t *pc = code + st->pc;
    uint32_t ibase = st->int_base, sbase = st->str_base, depth = st->depth;
    int64_t *R = st->ibank + ibase;
    pnx_scell *S = st->sbank + sbase;
    const uint64_t budget = st->budget;
    uint64_t left = st->left, count = 0;
    uint32_t w = 0;
    pinax_err result = PINAX_OK;

#define FAULT(error)                                                                               \
    do {                                                                                           \
        result = (error);                                                                          \
        goto finished;                                                                             \
    } while (0)

#if defined(PINAX_BUDGET_EACH)
#define BUDGET_STEP()                                                                              \
    do {                                                                                           \
        ++count;                                                                                   \
        if (budget) {                                                                              \
            if (left == 0)                                                                         \
                goto suspended;                                                                    \
            --left;                                                                                \
        }                                                                                          \
    } while (0)
#else
#define BUDGET_STEP() ((void)0)
#endif

#if defined(PINAX_SWITCH_DISPATCH)
#define CASE(name) case PNX_##name:
#define NEXT() continue
    for (;;) {
        BUDGET_STEP();
        w = *pc++;
        switch (w & 0xffu) {
#else
    static const void *const labels[PNX_OP_COUNT] = {
#define PNX_LABEL(name, a, b, c, shape, second, fails) [PNX_##name] = &&L_##name,
        PNX_OPS(PNX_LABEL)
#undef PNX_LABEL
    };
#define CASE(name) L_##name:
#define NEXT()                                                                                     \
    do {                                                                                           \
        BUDGET_STEP();                                                                             \
        w = *pc++;                                                                                 \
        goto *labels[w & 0xffu];                                                                   \
    } while (0)
    NEXT();
    {
        {
#endif

        // Plain instructions: the function does the work; a result other than 0 is a fault.
#define RUN(name)                                                                                  \
    CASE(name) {                                                                                   \
        const int r = pnx_op_##name(st, R, S, w, 0);                                               \
        if (r != PNX_R_OK)                                                                         \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }
        // Instructions with a second word.
#define RUN2(name)                                                                                 \
    CASE(name) {                                                                                   \
        const int r = pnx_op_##name(st, R, S, w, *pc);                                             \
        ++pc;                                                                                      \
        if (r != PNX_R_OK)                                                                         \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }
        // Instructions that can fail: on success the next word (the failure word) is skipped.
#define RUNF(name)                                                                                 \
    CASE(name) {                                                                                   \
        const int r = pnx_op_##name(st, R, S, w, 0);                                               \
        if (r == PNX_R_OK)                                                                         \
            ++pc;                                                                                  \
        else if (r != PNX_R_FAILED)                                                                \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }
        // Compare and branch: the function is the condition.
#define RUNJ(name)                                                                                 \
    CASE(name) {                                                                                   \
        if (pnx_cond_##name(st, R, S, w))                                                          \
            pc += OP_SC(w);                                                                        \
        NEXT();                                                                                    \
    }

        RUN(MOVI)
        RUN(MOVS)
        RUN(LOADI)
        RUN(LOADK)
        RUN(LOADKS)
        RUN(ADD)
        RUN(SUB)
        RUN(MUL)
        RUN(DIV)
        RUN(MOD)
        RUN(NEG)
        RUN(ADDI)
        RUN(WADD)
        RUN(WSUB)
        RUN(WMUL)
        RUN(AND)
        RUN(OR)
        RUN(XOR)
        RUN(SHL)
        RUN(SHR)
        RUN(USHR)
        RUNJ(JEQ)
        RUNJ(JNE)
        RUNJ(JLT)
        RUNJ(JLE)
        RUNJ(JEQI)
        RUNJ(JNEI)
        RUNJ(JLTI)
        RUNJ(JGEI)
        CASE(JMP) { pc += OP_SAX(w); NEXT(); }
        RUN(GETG)
        RUN(SETG)
        RUN(AGET)
        RUN(ASET)
        RUN(ROW)
        RUN2(FGET)
        RUN2(FSET)
        RUN2(FGETS)
        RUN2(FSETS)
        RUN(SLEN)
        RUN(SBYTE)
        RUNJ(JSEQ)
        RUNF(SNEXT)
        RUN(SCUT)
        RUNF(TOINT)
        RUN(BCLEAR)
        RUNF(BPUT)
        RUNF(BPUTS)
        RUN(BVIEW)
        RUNF(SGET)
        RUNF(SGETS)
        RUNJ(JSNE)
        RUN(GETFAIL)
        RUN(RMARK)
        RUN(RCATS)
        RUN(RCATI)
        RUN(RCATB)

        CASE(CALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            const uint32_t new_ibase = ibase + OP_A(w), new_sbase = sbase + OP_B(w);
            if (depth == st->depth_max || new_ibase + callee->int_cells > st->int_slots ||
                new_sbase + callee->str_cells > st->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            st->ctrl[depth++] = (pnx_frame){(uint32_t)(pc - code), ibase, sbase, 0};
            if (depth > st->depth_peak)
                st->depth_peak = depth;
            ibase = new_ibase;
            sbase = new_sbase;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            pc = code + callee->entry;
            NEXT();
        }
        CASE(TCALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            if (ibase + callee->int_cells > st->int_slots || sbase + callee->str_cells > st->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            pc = code + callee->entry;
            NEXT();
        }
        CASE(RET) {
            if (depth == 0)
                goto finished;
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc;
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(RETOK) { // success of a word that may fail: skip the caller's failure word
            if (depth == 0)
                goto finished;
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc + 1;
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(FAIL) {
            st->fail = (pinax_err)OP_BX(w);
            goto fail_return;
        }
        CASE(FAILR) {
            st->fail = (pinax_err)R[OP_A(w)];
            goto fail_return;
        }
        CASE(RETF) {
        fail_return:
            if (depth == 0)
                FAULT(st->fail);
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc; // the caller's failure word
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(HCALL) {
            bool fault;
            const pinax_err err = host_call(st, st->imports[*pc], R + OP_A(w), S + OP_B(w), &fault);
            ++pc;
            if (err != PINAX_OK)
                FAULT(err);
            NEXT();
        }
        CASE(HCALLF) {
            bool fault;
            const pinax_err err = host_call(st, st->imports[*pc], R + OP_A(w), S + OP_B(w), &fault);
            ++pc;
            if (err == PINAX_OK)
                ++pc;
            else if (fault)
                FAULT(err);
            else
                st->fail = err;
            NEXT();
        }

        CASE(CHECK) {
#if !defined(PINAX_BUDGET_EACH)
            const uint32_t n = OP_BX(w);
            count += n;
            if (budget) {
                if (left < n) {
                    --pc;
                    st->owed = n; // the next resume gets at least this much, so it moves on
                    goto suspended;
                }
                left -= n;
            }
#endif
            NEXT();
        }
        CASE(FAULT) { FAULT((pinax_err)OP_BX(w)); }

#if defined(PINAX_SWITCH_DISPATCH)
        default:
            FAULT(PINAX_ERR_FORMAT);
        }
    }
#else
        }
    }
#endif

suspended:
    st->pc = (uint32_t)(pc - code);
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth = depth;
    st->left = 0;
    st->instructions += count;
    st->suspended = true;
    return PINAX_SUSPENDED;

finished:
    st->left = left;
    st->instructions += count;
    st->suspended = false;
    st->depth = 0;
    return result;
}

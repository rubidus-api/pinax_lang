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

// ---- tasks ---------------------------------------------------------------------------------

static void ctx_save(const pinax_state *st, pnx_context *c) {
    *c = (pnx_context){st->ibank,     st->sbank,       st->ctrl,     st->int_slots,   st->str_slots,
                       st->depth_max, st->region_used, st->region_len, st->pc,        st->int_base,
                       st->str_base,  st->depth,       st->depth_floor, st->resume_dst, st->ends_task};
}

static void ctx_load(pinax_state *st, const pnx_context *c) {
    st->ibank = c->ibank;
    st->sbank = c->sbank;
    st->ctrl = c->ctrl;
    st->int_slots = c->int_slots;
    st->str_slots = c->str_slots;
    st->depth_max = c->depth_max;
    st->region_used = c->region_used;
    st->region_len = c->region_len;
    st->pc = c->pc;
    st->int_base = c->int_base;
    st->str_base = c->str_base;
    st->depth = c->depth;
    st->depth_floor = c->depth_floor;
    st->resume_dst = c->resume_dst;
    st->ends_task = c->ends_task;
}

// The task a value names, or NULL when that task has ended.
static pnx_task *task_of(const pnx_global *g, int64_t value) {
    const uint64_t slot = ((uint64_t)value & 0xffffffffu) - 1;
    if (slot >= g->capacity)
        return NULL;
    pnx_task *t = (pnx_task *)(void *)g->ptr + slot;
    return t->state != PNX_TASK_FREE && t->generation == (uint64_t)value >> 32 ? t : NULL;
}

// The run goes to task `t`. The state holds where the resumer stands (its pc already set to
// where it goes on); `dst` is the resumer's register for "the task has ended".
static void task_enter(pinax_state *st, pnx_task *t, uint32_t dst) {
    st->resume_dst = dst;
    ctx_save(st, st->task ? &st->task->ctx : &st->main);
    t->parent = st->task;
    t->in_host = st->nest;
    t->state = PNX_TASK_RUNNING;
    st->task = t;
    ctx_load(st, &t->ctx);
    if (t->wake_fail != PINAX_OK) { // the operation it waited for failed: its failure word is next
        st->fail = t->wake_fail;
        t->wake_fail = PINAX_OK;
    }
}

// The running task gives the run back to its resumer: it yielded, or (`ended`) it is over.
static void task_leave(pinax_state *st, bool ended) {
    pnx_task *t = st->task;
    if (ended) {
        t->state = PNX_TASK_FREE;
        ++t->generation;
    } else {
        ctx_save(st, &t->ctx);
        t->state = PNX_TASK_READY;
    }
    st->task = t->parent;
    ctx_load(st, st->task ? &st->task->ctx : &st->main);
    if (st->resume_dst != UINT32_MAX)
        st->ibank[st->int_base + st->resume_dst] = ended;
}

void pnx_tasks_abort(pinax_state *st) {
    if (st->task) {
        st->task = NULL;
        ctx_load(st, &st->main);
    }
    st->main_wait.token = 0;
    for (uint32_t k = 0; st->head && k < st->head->n_globals; ++k) {
        const pnx_global *g = &st->globals[k];
        if (g->kind != PNX_G_TASKS)
            continue;
        for (uint32_t slot = 0; slot < g->capacity; ++slot) {
            pnx_task *t = (pnx_task *)(void *)g->ptr + slot;
            if (t->state != PNX_TASK_FREE) {
                t->state = PNX_TASK_FREE;
                ++t->generation;
            }
        }
    }
}

// Calls a host word. Arguments start at ia / sa; results follow the arguments. Returns the
// host's outcome; *fault is set when the interpreter itself cannot go on.
static pinax_err host_call(pinax_state *st, const pnx_host *h, int64_t *ia, pnx_scell *sa, bool *fault,
                           uint64_t *token) {
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
    pinax_call call = {st, 0};
    // a word run from inside this host word puts its window above these arguments and results
    st->host_cur = h;
    st->host_ia = ia;
    st->host_sa = sa;
    pinax_err err = h->fn(&call, h->user, &args);
    st->host_cur = NULL;
    *fault = false;
    if (err == PINAX_PENDING && call.token && (h->flags & PINAX_WORD_AWAITABLE) && st->nest == 0) {
        *token = call.token; // the result comes later, through pinax_complete()
        return PINAX_PENDING;
    }
    if (err != PINAX_OK) {
        if (err < PINAX_ERR_FAILED) { // pending without a token, from a word not registered awaitable,
            *fault = true;            // or where a host word's C stack is in the way
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
    uint32_t floor = st->depth_floor;
    int64_t *R = st->ibank + ibase;
    pnx_scell *S = st->sbank + sbase;
    const uint64_t budget = st->budget;
    uint64_t left = st->left, count = 0;
    uint32_t w = 0, gl = 0; // gl: the global of a task instruction, narrow or wide
    pinax_err result = PINAX_OK;

#define FAULT(error)                                                                               \
    do {                                                                                           \
        result = (error);                                                                          \
        goto finished;                                                                             \
    } while (0)
    // Where the run stands goes into the state before a task takes over, and comes back out of
    // it when another one is in place.
#define SAVE_CTX()                                                                                 \
    do {                                                                                           \
        st->pc = (uint32_t)(pc - code);                                                            \
        st->int_base = ibase;                                                                      \
        st->str_base = sbase;                                                                      \
        st->depth = depth;                                                                         \
        st->depth_floor = floor;                                                                   \
    } while (0)
#define LOAD_CTX()                                                                                 \
    do {                                                                                           \
        pc = code + st->pc;                                                                        \
        ibase = st->int_base;                                                                      \
        sbase = st->str_base;                                                                      \
        depth = st->depth;                                                                         \
        floor = st->depth_floor;                                                                   \
        R = st->ibank + ibase;                                                                     \
        S = st->sbank + sbase;                                                                     \
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
#define PNX_LABEL(name, number, a, b, c, shape, second, fails, roles) [PNX_##name] = &&L_##name,
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
// An instruction that names a global: C in the narrow form, the second word in the wide one.
#define RUNG(name)                                                                                 \
    CASE(name) {                                                                                   \
        const int r = pnx_op_##name(st, R, S, w, OP_C(w));                                         \
        if (r != PNX_R_OK)                                                                         \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }                                                                                              \
    CASE(name##W) {                                                                                \
        const int r = pnx_op_##name(st, R, S, w, *pc);                                             \
        ++pc;                                                                                      \
        if (r != PNX_R_OK)                                                                         \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }
#define RUNFG(name)                                                                                \
    CASE(name) {                                                                                   \
        const int r = pnx_op_##name(st, R, S, w, OP_C(w));                                         \
        if (r == PNX_R_OK)                                                                         \
            ++pc;                                                                                  \
        else if (r != PNX_R_FAILED)                                                                \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }                                                                                              \
    CASE(name##W) {                                                                                \
        const int r = pnx_op_##name(st, R, S, w, *pc);                                             \
        ++pc;                                                                                      \
        if (r == PNX_R_OK)                                                                         \
            ++pc;                                                                                  \
        else if (r != PNX_R_FAILED)                                                                \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }
#define RUNF2(name)                                                                                \
    CASE(name) {                                                                                   \
        const int r = pnx_op_##name(st, R, S, w, *pc);                                             \
        ++pc;                                                                                      \
        if (r == PNX_R_OK)                                                                         \
            ++pc;                                                                                  \
        else if (r != PNX_R_FAILED)                                                                \
            FAULT((pinax_err)r);                                                                   \
        NEXT();                                                                                    \
    }
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
        RUN(MULI)
        RUN(DIVI)
        RUN(MODI)
        RUN(ANDI)
        RUN(ORI)
        RUN(XORI)
        RUN(SHLI)
        RUN(SHRI)
        RUN(USHRI)
        RUN(WMULI)
        RUN(DRESC)
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
        RUNG(AGET)
        RUNG(ASET)
        RUNG(ROW)
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
        RUNFG(BPUT)
        RUNFG(BPUTS)
        RUN(BVIEW)
        RUNF(SGET)
        RUNF(SGETS)
        RUNJ(JSNE)
        RUN(GETFAIL)
        RUN(RMARK)
        RUN(RCATS)
        RUN(RCATI)
        RUN(RCATB)
        RUN(RCATD)
        RUNF(STODEC)
        RUN(GLEN)
        RUN(GCLEAR)
        RUN(GCAP)
        RUNF(LPUSH)
        RUNF(LPOP)
        RUNF(LPUSHS)
        RUNF(LPOPS)
        RUNF2(LCOPYR)
        RUNF(SFIND)
        RUNF(SWORD)
        RUN(ERRTEXT)
        RUNF(ADDROW)
        RUNF(BCAT)
        RUNF(BCATI)
        RUN(STAKE)
        RUN(SSKIP)
        RUN(STAKEL)
        RUN(SSKIPL)
        RUN(SPRE)
        RUN(SSUF)
        RUN(SHAS)
        RUNFG(IXGET)
        RUNFG(IXGETS)
        RUNF(IXPUT)
        RUN(IXDEL)
        RUN2(SREPL)
        RUNF(QPUT)
        RUNF(QTAKE)
        RUNF(QPEEK)
        RUNF(PALLOC)
        RUN(PFREE)
        RUNG(PALIVE)
        RUN(SETFAIL)
        RUN(RTOP)
        RUN(RCUT)
        RUN(RFREE)
        RUN(SCUTS)
        RUNF(SCUTSF)
        RUN(STRIM)
        RUNF(SLINE)
        RUNF(SNEXTS)
        RUNF(SFINDL)
        RUN(SCMP)
        RUN(RCATX)
        RUN(RCATW)
        RUN(RCATXW)
        RUN(RPADL)
        RUN(RPADR)
        RUN(SUTF)
        RUNF(TOINTB)
        RUN(SSPAN)
        RUNF(SCHAR)
        RUN(RCASE)
        RUN(RREP)
        RUN(SHASH)
        RUNF(PMATCH)
        RUN(PCAP)
        RUNF(PNEXT)
        RUN2(PREPL)
        RUNF(XMATCH)
        RUNF(XCUT)
        RUNF2(XNEXT)
        RUNF2(XSPLIT)
        RUN(XCAPI)
        RUNF2(XSTEP)
        RUN2(XREPL)
        RUNF(XCOMP)
        RUNFG(BENC)
        RUN(BTRUNC)
        RUNF(FITW)
        RUNF(JNEXT)
        RUNF(JSKIP)
        RUNFG(JPUTI)
        RUNFG(JPUTS)
        RUNF(CNEXT)
        RUNFG(CPUTS)
        RUNFG(CPUTI)
        RUN(VLEN)
        RUN(SATADD)
        RUN(SATSUB)
        RUN(SATMUL)
        RUN(UDIV)
        RUN(UMOD)
        RUN(ROTL)
        RUN(ROTR)
        RUN(MULHI)
        RUN(UMULHI)
        RUN(FDIV)
        RUN(FMOD)
        RUN(ULT)
        RUN(ULE)
        RUN(ABS)
        RUN(BNOT)
        RUN(POPC)
        RUN(CLZ)
        RUN(CTZ)
        RUN(BSWAP)
        RUNF(CHKADD)
        RUNF(CHKSUB)
        RUNF(CHKMUL)
        RUNF(CHKDIV)

        CASE(CALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            const uint32_t new_ibase = ibase + OP_A(w), new_sbase = sbase + OP_B(w);
            if (depth == st->depth_max || new_ibase + callee->int_cells > st->int_slots ||
                new_sbase + callee->str_cells > st->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            st->ctrl[depth++] = (pnx_frame){(uint32_t)(pc - code), ibase, sbase, 0};
            if (depth > st->depth_peak)
                st->depth_peak = depth;
            if (new_ibase + callee->int_cells > st->int_peak)
                st->int_peak = new_ibase + callee->int_cells;
            if (new_sbase + callee->str_cells > st->str_peak)
                st->str_peak = new_sbase + callee->str_cells;
            ibase = new_ibase;
            sbase = new_sbase;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            pc = code + callee->entry;
            NEXT();
        }
        CASE(CALLR) { // the word number comes from a register; what it takes and gives is checked here
            const uint32_t expect = *pc++;
            const uint64_t number = (uint64_t)R[OP_C(w)];
            if (number >= st->head->n_words)
                FAULT(PINAX_ERR_RANGE);
            const pnx_counts have = st->word_counts[number];
            if (have.int_args != (expect & 0xffu) || have.str_args != ((expect >> 8) & 0xffu) ||
                have.int_results != ((expect >> 16) & 0xffu) || have.str_results != ((expect >> 24) & 0x7fu) ||
                have.fails != (expect >> 31 != 0))
                FAULT(PINAX_ERR_SIGNATURE);
            const pnx_wordrec *callee = &st->words[number];
            const uint32_t new_ibase = ibase + OP_A(w), new_sbase = sbase + OP_B(w);
            if (depth == st->depth_max || new_ibase + callee->int_cells > st->int_slots ||
                new_sbase + callee->str_cells > st->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            st->ctrl[depth++] = (pnx_frame){(uint32_t)(pc - code), ibase, sbase, 0};
            if (depth > st->depth_peak)
                st->depth_peak = depth;
            if (new_ibase + callee->int_cells > st->int_peak)
                st->int_peak = new_ibase + callee->int_cells;
            if (new_sbase + callee->str_cells > st->str_peak)
                st->str_peak = new_sbase + callee->str_cells;
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
            if (ibase + callee->int_cells > st->int_peak)
                st->int_peak = ibase + callee->int_cells;
            if (sbase + callee->str_cells > st->str_peak)
                st->str_peak = sbase + callee->str_cells;
            pc = code + callee->entry;
            NEXT();
        }
        CASE(RET) {
            if (depth == floor) {
                if (!st->ends_task)
                    goto finished;
                SAVE_CTX(); // the word of a task returned: the task is over
                task_leave(st, true);
                LOAD_CTX();
                NEXT();
            }
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc;
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        CASE(RETOK) { // success of a word that may fail: skip the caller's failure word
            if (depth == floor)
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
            if (depth == floor)
                FAULT(st->fail);
            const pnx_frame f = st->ctrl[--depth];
            pc = code + f.pc; // the caller's failure word
            ibase = f.int_base;
            sbase = f.str_base;
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            NEXT();
        }
        // The host word left its operation pending: this run waits. A task gives the run back
        // to its resumer; the main run gives it back to the host.
#define AWAIT(with_failure_word)                                                                   \
    do {                                                                                           \
        pnx_wait *wait = st->task ? &st->task->wait : &st->main_wait;                              \
        *wait = (pnx_wait){token, st->imports[pc[-1]], ibase + OP_A(w), sbase + OP_B(w), with_failure_word}; \
        if (!st->task)                                                                             \
            goto waiting;                                                                          \
        SAVE_CTX();                                                                                \
        task_leave(st, false);                                                                     \
        wait_task->state = PNX_TASK_WAITING;                                                       \
        LOAD_CTX();                                                                                \
        NEXT();                                                                                    \
    } while (0)
        CASE(HCALL) {
            bool fault;
            uint64_t token = 0;
            st->depth = depth; // a word run from inside the host word goes on from here
            st->left = left;
            const pinax_err err = host_call(st, st->imports[*pc], R + OP_A(w), S + OP_B(w), &fault, &token);
            left = st->left;
            ++pc;
            if (err == PINAX_PENDING) {
                pnx_task *const wait_task = st->task;
                AWAIT(false);
            }
            if (err != PINAX_OK)
                FAULT(err);
            NEXT();
        }
        CASE(HCALLF) {
            bool fault;
            uint64_t token = 0;
            st->depth = depth;
            st->left = left;
            const pinax_err err = host_call(st, st->imports[*pc], R + OP_A(w), S + OP_B(w), &fault, &token);
            left = st->left;
            ++pc;
            if (err == PINAX_PENDING) { // it goes on at the failure word, or past it when the result is good
                pnx_task *const wait_task = st->task;
                AWAIT(true);
            }
            if (err == PINAX_OK)
                ++pc;
            else if (fault)
                FAULT(err);
            else
                st->fail = err;
            NEXT();
        }

        CASE(TSPAWNW) {
            gl = *pc++;
            goto spawn;
        }
        CASE(TSPAWN) { // the next word runs when no slot is free
            gl = OP_C(w);
        spawn:;
            const pnx_global *g = &st->globals[gl];
            const uint64_t number = (uint64_t)R[OP_B(w)];
            if (number >= st->head->n_words)
                FAULT(PINAX_ERR_RANGE);
            const pnx_counts have = st->word_counts[number];
            const pnx_wordrec *callee = &st->words[number];
            if (have.int_args || have.str_args || have.int_results || have.str_results || have.fails)
                FAULT(PINAX_ERR_SIGNATURE);
            pnx_task *t = NULL;
            for (uint32_t slot = 0; slot < g->capacity && !t; ++slot) {
                pnx_task *each = (pnx_task *)(void *)g->ptr + slot;
                if (each->state == PNX_TASK_FREE)
                    t = each;
            }
            if (!t) {
                st->fail = PINAX_ERR_FULL;
                NEXT();
            }
            if (callee->int_cells > t->int_slots || callee->str_cells > t->str_slots)
                FAULT(PINAX_ERR_DEPTH);
            ++t->generation; // odd: in use
            t->state = PNX_TASK_READY;
            t->cancelled = t->yielded = t->blocked = false;
            t->wake_fail = t->wake_fault = PINAX_OK;
            t->wait.token = 0;
            t->ctx = (pnx_context){t->ibank,     t->sbank,       t->ctrl, t->int_slots, t->str_slots,
                                   t->depth_max, t->region_base, t->region_base + t->region_bytes,
                                   callee->entry, 0, 0, 0, 0, UINT32_MAX, true};
            R[OP_A(w)] = (int64_t)((uint64_t)t->generation << 32 |
                                   (uint64_t)(t - (pnx_task *)(void *)g->ptr + 1));
            ++pc;
            NEXT();
        }
        CASE(TRESUMEW) {
            gl = *pc++;
            goto resume;
        }
        CASE(TRESUME) {
            gl = OP_C(w);
        resume:;
            pnx_task *t = task_of(&st->globals[gl], R[OP_B(w)]);
            if (!t || t->state == PNX_TASK_WAITING) { // it has ended; or it waits for the host and cannot go on
                R[OP_A(w)] = !t;
                NEXT();
            }
            if (t->state != PNX_TASK_READY) // itself, or one that waits for this one
                FAULT(PINAX_ERR_STATE);
            SAVE_CTX();
            task_enter(st, t, OP_A(w));
            LOAD_CTX();
            if (t->wake_fault != PINAX_OK)
                FAULT(t->wake_fault);
            NEXT();
        }
        CASE(TYIELD) { // the next word runs when the task was cancelled
            pnx_task *t = st->task;
            if (!t) { // the main run has no one to yield to
                ++pc;
                NEXT();
            }
            if (t->yielded) { // back from the yield
                t->yielded = false;
                if (t->cancelled) {
                    st->fail = PINAX_ERR_CANCELLED;
                    NEXT();
                }
                ++pc;
                NEXT();
            }
            if (st->nest != t->in_host) // a host word's C stack is in the way
                FAULT(PINAX_ERR_STATE);
            t->yielded = true;
            --pc; // the task goes on from this instruction
            SAVE_CTX();
            task_leave(st, false);
            LOAD_CTX();
            NEXT();
        }
        CASE(GSEL) { // the global a reference names, for the instruction after this one
            const uint64_t gi = (uint64_t)R[OP_A(w)];
            const uint32_t elem = *pc++;
            if (gi >= st->head->n_globals || st->globals[gi].kind != OP_B(w) ||
                (elem && st->globals[gi].elem_bytes != elem) || st->globals[gi].shared)
                FAULT(PINAX_ERR_STATE); // a shared global is never a parameter: the word may write it
            st->gsel = (uint32_t)gi;
            NEXT();
        }
        CASE(QWTAKE) { // the front of a queue, yielding while it is empty
            if (st->globals[PNX_GX(st, OP_BX(w))].length == 0)
                goto queue_blocked;
            (void)pnx_op_QTAKE(st, R, S, w, 0);
            goto queue_went;
        }
        CASE(QWPUT) { // to the back of a queue, yielding while it is full
            if (st->globals[PNX_GX(st, OP_BX(w))].length == st->globals[PNX_GX(st, OP_BX(w))].capacity)
                goto queue_blocked;
            (void)pnx_op_QPUT(st, R, S, w, 0);
        queue_went:
            if (st->task)
                st->task->yielded = st->task->blocked = false;
            ++pc; // past the failure word
            NEXT();
        queue_blocked:;
            pnx_task *t = st->task;
            if (!t) // the main run waits for a queue: nothing else can run to change it
                FAULT(PINAX_ERR_DEADLOCK);
            if (t->yielded) { // back from waiting, and it still cannot go
                t->yielded = t->blocked = false;
                if (t->cancelled) {
                    st->fail = PINAX_ERR_CANCELLED;
                    NEXT();
                }
            }
            if (st->nest != t->in_host) // a host word's C stack is in the way
                FAULT(PINAX_ERR_STATE);
            t->yielded = t->blocked = true;
            t->blocked_at = st->qturn; // it cannot go until some queue changes
            pc -= OP_BX(w) == PNX_GSELECTED ? 3 : 1; // goes on from this instruction, or the GSEL before it
            SAVE_CTX();
            task_leave(st, false);
            LOAD_CTX();
            NEXT();
        }
        CASE(TDONE) {
            R[OP_A(w)] = task_of(&st->globals[OP_C(w)], R[OP_B(w)]) == NULL;
            NEXT();
        }
        CASE(TDONEW) {
            R[OP_A(w)] = task_of(&st->globals[*pc++], R[OP_B(w)]) == NULL;
            NEXT();
        }
        CASE(TCANCEL) {
            pnx_task *t = task_of(&st->globals[OP_BX(w)], R[OP_A(w)]);
            if (t)
                t->cancelled = true;
            NEXT();
        }
        CASE(TRUNALL) { // comes back here after every task's turn, until none is left
            pnx_global *g = &st->globals[OP_BX(w)];
            pnx_task *t = NULL;
            bool any_waits = false, any_blocked = false;
            for (uint32_t k = 0; k < g->capacity && !t; ++k) {
                const uint32_t slot = (g->head + 1 + k) % g->capacity;
                pnx_task *each = (pnx_task *)(void *)g->ptr + slot;
                any_waits = any_waits || each->state == PNX_TASK_WAITING;
                if (each->state != PNX_TASK_READY)
                    continue;
                if (each->blocked && each->blocked_at == st->qturn && !each->cancelled) {
                    any_blocked = true; // no queue changed since it stopped: it would stop again
                    continue;
                }
                t = each;
                g->head = slot;
            }
            if (!t && !any_waits) {
                if (any_blocked) // every task left waits for a queue, and none can change one
                    FAULT(PINAX_ERR_DEADLOCK);
                NEXT();
            }
            --pc;
            if (!t) { // every task left waits for the host (some perhaps for a queue too): so does the state
                if (st->nest)
                    FAULT(PINAX_ERR_STATE);
                goto waiting;
            }
            SAVE_CTX();
            task_enter(st, t, UINT32_MAX);
            LOAD_CTX();
            if (t->wake_fault != PINAX_OK)
                FAULT(t->wake_fault);
            NEXT();
        }

        CASE(CHECK) {
#if !defined(PINAX_BUDGET_EACH)
            uint64_t n = OP_BX(w);
            const uint64_t charge = st->charge;
            if (charge >= 16) // bytes scanned since the last CHECK (D0022: 16 bytes as one instruction)
                n += charge >> 4;
            if (budget) {
                if (left < n) {
                    --pc;
                    st->owed = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n; // the next resume gets at least this much
                    goto suspended;
                }
                left -= n;
            }
            count += n;
            if (charge >= 16)
                st->charge = charge & 15u;
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

waiting: // like suspended; the host goes on with pinax_resume() after pinax_complete()
    st->waiting = true;
    st->pc = (uint32_t)(pc - code);
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth = depth;
    st->left = left;
    st->instructions += count;
    st->suspended = true;
    return PINAX_WAITING;

suspended:
    st->pc = (uint32_t)(pc - code);
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth = depth;
    st->left = 0;
    st->instructions += count;
    st->suspended = true;
    if (st->region_used > st->region_peak)
        st->region_peak = st->region_used;
    return PINAX_SUSPENDED;

finished:
    if (result != PINAX_OK) {
        st->fault_pc = (uint32_t)(pc - code);
        st->fault_ibase = ibase;
        st->fault_sbase = sbase;
        // a fault inside a task ends the run and every task; so does a deadlock, whose tasks could never go on
        if ((st->task || result == PINAX_ERR_DEADLOCK) && st->nest == 0)
            pnx_tasks_abort(st);
    }
    st->left = left;
    st->instructions += count;
    st->suspended = false;
    st->depth = floor;
    if (st->region_used > st->region_peak)
        st->region_peak = st->region_used;
    return result;
}

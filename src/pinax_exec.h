// pinax_exec.h - the body of the interpreter loop, included twice by pinax_vm.c (RFC-0028):
// PNX_EXEC_STEP 0 makes pnx_execute, which runs until the run ends, stops or waits; PNX_EXEC_STEP 1
// makes pnx_step, which runs one instruction and gives back PNX_STEP_ON with where the run stands
// in the state, so that exported code (pinax_export.c) hands the interpreter the instructions it
// does not do itself and goes on. One body, so the two cannot differ.
// Not a header to include anywhere else: it uses the static functions of pinax_vm.c.

pinax_err PNX_EXEC_NAME(pinax_state *st) {
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
#if PNX_EXEC_STEP
#define NEXT() goto stepped
#else
#define NEXT() continue
#endif
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
#define FIRST()                                                                                    \
    do {                                                                                           \
        BUDGET_STEP();                                                                             \
        w = *pc++;                                                                                 \
        goto *labels[w & 0xffu];                                                                   \
    } while (0)
#if PNX_EXEC_STEP // pnx_step: one instruction, then back to the caller
#define NEXT() goto stepped
#else
#define NEXT() FIRST()
#endif
    FIRST();
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

        // every instruction that is one call of its pnx_op_ or pnx_cond_ function (PNX_RUN_KINDS)
#define PNX_EXPAND(kind, name) kind(name)
        PNX_RUN_KINDS(PNX_EXPAND)
#undef PNX_EXPAND
        CASE(JMP) { pc += OP_SAX(w); NEXT(); }

        CASE(CALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            const int r = pnx_enter_word(st, callee, w, (uint32_t)(pc - code), &ibase, &sbase, &depth);
            if (r != PNX_R_OK)
                FAULT((pinax_err)r);
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
            const int r = pnx_enter_word(st, callee, w, (uint32_t)(pc - code), &ibase, &sbase, &depth);
            if (r != PNX_R_OK)
                FAULT((pinax_err)r);
            R = st->ibank + ibase;
            S = st->sbank + sbase;
            pc = code + callee->entry;
            NEXT();
        }
        CASE(TCALL) {
            const pnx_wordrec *callee = &st->words[*pc++];
            const int r = pnx_tail_word(st, callee, ibase, sbase);
            if (r != PNX_R_OK)
                FAULT((pinax_err)r);
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
            t->ptn = (pnx_ptn_found){0}; // a capture is a view of what this task matched
            t->ctx = (pnx_context){t->ibank,       t->sbank,     t->ctrl, t->int_slots, t->str_slots,
                                   t->depth_max,   t->region_base, t->region_base + t->region_bytes,
                                   t->region_base, callee->entry, 0, 0, 0, 0, UINT32_MAX, true};
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
            const uint32_t elem = *pc++, next = *pc; // the loader keeps the instruction after a GSEL in the word
            // a shared global is chosen only to be read: LCOPYR writes the one in its BX
            const bool writes = pnx_writes_global(next & 0xffu) && ((next & 0xffu) != PNX_LCOPYR || OP_BX(next) == PNX_GSELECTED);
            if (gi >= st->head->n_globals || st->globals[gi].kind != OP_B(w) ||
                (elem && st->globals[gi].elem_bytes != (elem & 0xffffu)) ||
                ((elem >> 16) && st->globals[gi].width != (elem >> 16) - 1) || (writes && st->globals[gi].shared))
                FAULT(PINAX_ERR_STATE);
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

#if PNX_EXEC_STEP
stepped: // the one instruction is done: where the run stands goes back into the state, and the caller goes on
    st->pc = (uint32_t)(pc - code);
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth = depth;
    st->depth_floor = floor;
    st->left = left;
    st->instructions += count;
    return PNX_STEP_ON;
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
    return pnx_exec_suspend(st, (uint32_t)(pc - code), ibase, sbase, depth, count);

finished:
    if (result != PINAX_OK && pnx_fault_apart(st, result, (uint32_t)(pc - code), ibase, sbase)) {
        LOAD_CTX(); // a task of an `apart` set faulted: it alone ends (RFC-0029)
        result = PINAX_OK;
        NEXT();
    }
    return pnx_exec_end(st, result, (uint32_t)(pc - code), ibase, sbase, R, floor, left, count);
}

#undef FAULT
#undef SAVE_CTX
#undef LOAD_CTX
#undef BUDGET_STEP
#undef CASE
#undef FIRST
#undef NEXT
#undef RUN
#undef RUN2
#undef RUNG
#undef RUNFG
#undef RUNF2
#undef RUNF
#undef RUNJ
#undef AWAIT
#undef PNX_EXEC_NAME
#undef PNX_EXEC_STEP

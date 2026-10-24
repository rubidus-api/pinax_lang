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
    *c = (pnx_context){st->ibank,       st->sbank,       st->ctrl,       st->int_slots,   st->str_slots,
                       st->depth_max,   st->region_used, st->region_len, st->region_floor, st->pc,
                       st->int_base,    st->str_base,    st->depth,      st->depth_floor, st->resume_dst,
                       st->ends_task};
}

void pnx_ctx_save(const pinax_state *st, pnx_context *c) { ctx_save(st, c); }

static void ctx_load(pinax_state *st, const pnx_context *c) {
    st->ibank = c->ibank;
    st->sbank = c->sbank;
    st->ctrl = c->ctrl;
    st->int_slots = c->int_slots;
    st->str_slots = c->str_slots;
    st->depth_max = c->depth_max;
    st->region_used = c->region_used;
    st->region_len = c->region_len;
    st->region_floor = c->region_floor;
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

void pnx_fault_banks(pinax_state *st) {
    st->fault_ibank = st->ibank;
    st->fault_sbank = st->sbank;
    st->fault_int_slots = st->int_slots;
    st->fault_str_slots = st->str_slots;
}

void pnx_tasks_unwind(pinax_state *st, pnx_task *task, const pnx_context *c) {
    while (st->task && st->task != task) { // each was resumed by the one before, inside that word
        pnx_task *t = st->task;
        st->task = t->parent;
        t->state = PNX_TASK_FREE;
        ++t->generation;
    }
    st->task = task;
    ctx_load(st, c);
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
    for (uint32_t k = 0; k < n.int_results && k < PNX_HOST_WIDTHS; ++k) { // a result of a narrow type (RFC-0025)
        if (!pnx_width_holds(ia[n.int_args + k], h->res_width[k])) {
            *fault = true;
            return PINAX_ERR_OVERFLOW;
        }
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
        } else if (h->flags & PINAX_WORD_KEEP_STR) { // the bytes outlive the state: no copy, no slot
            *cell = (pnx_scell){PNX_SEG_KEPT, len, (uint64_t)((uintptr_t)rs[k].ptr - (uintptr_t)st->segment[PNX_SEG_KEPT])};
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

#if defined(PINAX_CHECK_PROOFS)
// RFC-0023: whether the fault at `at` (the pc after the instruction) is the check the loader
// proved away for that instruction. A field instruction has other checks (the width of the
// field, the length of a str(N) cell) that the proof does not cover: the row check is done again.
static bool pnx_proven_fired(const pinax_state *st, const int64_t *R, uint32_t at, pinax_err result) {
    if (!st->proven || at == 0 ||
        (result != PINAX_ERR_RANGE && result != PINAX_ERR_OVERFLOW && result != PINAX_ERR_STALE))
        return false;
    uint32_t i = at - 1;
    if (st->proven[i] & 0x80u) // the second word of a two-word instruction
        --i;
    if (st->proven[i] == PNX_PROVEN_NONE)
        return false;
    const uint32_t w = st->code[i], op = w & 0xffu;
    switch (op) {
    case PNX_FGET: case PNX_FSET: case PNX_FGETS: case PNX_FSETS: {
        uint64_t row;
        return pnx_row_of(&st->globals[PNX_GF(st, st->code[i + 1])], R[OP_B(w)], &row) != PNX_R_OK;
    }
    case PNX_ADD: case PNX_SUB: case PNX_MUL: case PNX_NEG: case PNX_ADDI: case PNX_MULI:
        return result == PINAX_ERR_OVERFLOW;
    default:
        return result == PINAX_ERR_RANGE;
    }
}
#endif

// How a run ends: with a fault (where it happened, the banks for the fault's locals, every task
// gone when a task faulted) or done. pnx_execute, pnx_step and exported code (RFC-0028) end here.
pinax_err pnx_exec_end(pinax_state *st, pinax_err result, uint32_t at, uint32_t ibase, uint32_t sbase,
                       const int64_t *R, uint32_t floor, uint64_t left, uint64_t count) {
    (void)R;
    if (result != PINAX_OK) {
        st->fault_pc = at;
#if defined(PINAX_CHECK_PROOFS) // RFC-0023: a check the loader proved away must never fire
        if (pnx_proven_fired(st, R, st->fault_pc, result))
            __builtin_trap();
#endif
        st->fault_ibase = ibase;
        st->fault_sbase = sbase;
        pnx_fault_banks(st);
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

bool pnx_fault_apart(pinax_state *st, pinax_err result, uint32_t at, uint32_t ibase, uint32_t sbase) {
    if (!st->task || !st->task->apart || result == PINAX_ERR_DEADLOCK || st->nest != 0)
        return false;
    st->fault_pc = at;
#if defined(PINAX_CHECK_PROOFS) // RFC-0023: a check the loader proved away must never fire
    if (pnx_proven_fired(st, st->ibank + ibase, st->fault_pc, result))
        __builtin_trap();
#endif
    st->fault_ibase = ibase;
    st->fault_sbase = sbase;
    pnx_fault_banks(st);
    pnx_note_task_fault(st, result);
    task_leave(st, true); // the task is over; whoever resumed it goes on
    return true;
}

// The budget ran out at `at` (the CHECK, which runs again when the run goes on).
pinax_err pnx_exec_suspend(pinax_state *st, uint32_t at, uint32_t ibase, uint32_t sbase, uint32_t depth,
                           uint64_t count) {
    st->pc = at;
    st->int_base = ibase;
    st->str_base = sbase;
    st->depth = depth;
    st->left = 0;
    st->instructions += count;
    st->suspended = true;
    if (st->region_used > st->region_peak)
        st->region_peak = st->region_used;
    return PINAX_SUSPENDED;
}

#define PNX_EXEC_NAME pnx_execute
#define PNX_EXEC_STEP 0
#include "pinax_exec.h"

#define PNX_EXEC_NAME pnx_step
#define PNX_EXEC_STEP 1
#include "pinax_exec.h"

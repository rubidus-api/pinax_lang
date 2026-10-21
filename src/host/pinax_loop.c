// pinax_loop.c - the default host loop and channels (see pinax_loop.h). Written against pinax.h;
// threads and their semaphores come from the vendored proven. Memory: every object of the loop (a
// message, a completion, a waiter, a channel, a job, the loop itself) is taken through
// `loop_alloc` and given back through `loop_free`, which call the functions the host gave in
// pinax_loop_config, or proven's heap allocator when it gave none.
//
// Locks: each channel has one, the loop has one. A channel's lock may be held while the loop's is
// taken (to hand a run its completion); never the other way round.
#include "pinax_loop.h"

#include "../../vendor/proven/platform/proven_sys_thread.h"
#include "proven/heap.h"

#include <stdalign.h>
#include <string.h>

// The host gave no memory functions: proven's heap.
static void *heap_alloc(void *user, size_t bytes, size_t align) {
    (void)user;
    const proven_allocator_t heap = proven_heap_allocator();
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, bytes, align);
    return room.err == PROVEN_OK ? room.value.ptr : NULL;
}

static void heap_free(void *user, void *object) {
    (void)user;
    const proven_allocator_t heap = proven_heap_allocator();
    heap.free_fn(heap.ctx, object);
}

// Zeroed bytes from the loop's memory, aligned for any object of this file; NULL when there are none.
static void *mem_alloc(const pinax_loop_memory *mem, size_t bytes) {
    void *object = mem->alloc(mem->user, bytes, alignof(max_align_t));
    if (object)
        memset(object, 0, bytes);
    return object;
}

static void mem_free(const pinax_loop_memory *mem, void *object) {
    if (object)
        mem->free(mem->user, object);
}

typedef proven_sys_semaphore_t lock_t; // a semaphore of one permit

static bool lock_init(lock_t *l) {
    if (!proven_sys_semaphore_init(l))
        return false;
    if (!proven_sys_semaphore_post(l)) {
        proven_sys_semaphore_destroy(l);
        return false;
    }
    return true;
}
static void lock(lock_t *l) {
    while (!proven_sys_semaphore_wait(l)) {
    }
}
static void unlock(lock_t *l) {
    (void)proven_sys_semaphore_post(l);
}

static void *loop_alloc(const pinax_loop *loop, size_t bytes);
static void  loop_free(const pinax_loop *loop, void *object);

typedef struct message {
    struct message *next;
    size_t          len;
    unsigned char   bytes[];
} message;

static message *message_copy(const pinax_loop *loop, pinax_str text) {
    message *m = loop_alloc(loop, sizeof *m + text.len);
    if (m) {
        m->next = NULL;
        m->len = text.len;
        if (text.len)
            memcpy(m->bytes, text.ptr, text.len);
    }
    return m;
}

typedef struct job job;

// A run parked on a channel: a receiver, or a sender with its message, waiting for room.
typedef struct waiter {
    struct waiter *next;
    job           *job;
    pinax_token    token;
    message       *msg;
} waiter;

// What finishes one operation a run waits for, kept until the run's thread takes it.
typedef struct completion {
    struct completion *next;
    pinax_token        token;
    pinax_err          outcome;
    pinax_channel     *from; // a message handed over by this channel: given back when nobody waits for it
    message           *msg;
    size_t             ni, ns;
    int64_t           *ri; // in the same allocation, as are the strings' bytes
    pinax_str         *rs;
} completion;

enum { JOB_READY, JOB_RUNNING, JOB_PARKED };

struct job {
    job              *next_ready, *next_active;
    pinax_state      *state;
    pinax_word        word;
    const pinax_args *args;
    pinax_loop_done   done;
    void             *user;
    int               kind;
    bool              started, deadlocked;
    completion       *inbox, **inbox_tail;
};

struct pinax_channel {
    pinax_channel *next;
    pinax_loop    *loop;
    lock_t         lock;
    char           name[64];
    uint32_t       capacity, max_bytes, count;
    bool           closed;
    message       *head, *tail;
    waiter        *receivers, *senders; // first come, first served
    pinax_word_def defs[3];
};

struct pinax_loop {
    pinax_loop_memory      memory;
    lock_t                 lock;
    proven_sys_semaphore_t work, idle;
    proven_sys_thread_t   *threads;
    unsigned               n_threads;
    job                   *ready, **ready_tail, *active;
    size_t                 n_active, running, outstanding;
    bool                   stopping, waiting;
    pinax_channel         *channels;
};

static thread_local job *current; // the run this thread is in, while it is in one

static void *loop_alloc(const pinax_loop *loop, size_t bytes) {
    return mem_alloc(&loop->memory, bytes);
}

static void loop_free(const pinax_loop *loop, void *object) {
    mem_free(&loop->memory, object);
}

static void push_ready(pinax_loop *loop, job *j) { // loop locked
    j->kind = JOB_READY;
    j->next_ready = NULL;
    *loop->ready_tail = j;
    loop->ready_tail = &j->next_ready;
    (void)proven_sys_semaphore_post(&loop->work);
}

static job *find_job(pinax_loop *loop, const pinax_state *state) { // loop locked
    job *j = loop->active;
    while (j && j->state != state)
        j = j->next_active;
    return j;
}

// Every run left waits and nothing outside can wake one: they all end with PINAX_ERR_DEADLOCK.
// Only while the host is in pinax_loop_wait(): before that it may still start the run that sends.
static void check_deadlock(pinax_loop *loop) { // loop locked
    if (!loop->waiting || loop->running || loop->ready || loop->outstanding || !loop->active)
        return;
    for (job *j = loop->active; j; j = j->next_active) {
        j->deadlocked = true;
        push_ready(loop, j);
    }
}

// Hands a completion to the run of `j`: it is taken on that run's thread before the run goes on.
static void post(pinax_loop *loop, job *j, completion *c) {
    lock(&loop->lock);
    c->next = NULL;
    *j->inbox_tail = c;
    j->inbox_tail = &c->next;
    if (j->kind == JOB_PARKED)
        push_ready(loop, j);
    unlock(&loop->lock);
}

static completion *completion_new(const pinax_loop *loop, pinax_token token, pinax_err outcome) {
    completion *c = loop_alloc(loop, sizeof *c);
    if (c)
        c->token = token, c->outcome = outcome;
    return c;
}

// ---- channels -----------------------------------------------------------------------------

static void give_back(pinax_channel *ch, message *m);

// Wakes a parked run with an outcome and, for a receiver, its message. Without memory for the
// completion the run would wait for ever; a sender's message is given back instead of lost.
static void wake(pinax_channel *ch, waiter *w, pinax_err outcome, message *m) {
    completion *c = completion_new(ch->loop, w->token, outcome);
    if (c) {
        c->msg = m;
        c->from = m ? ch : NULL;
        post(ch->loop, w->job, c);
    } else {
        loop_free(ch->loop, m);
    }
    loop_free(ch->loop, w);
}

static void append(pinax_channel *ch, message *m) { // channel locked
    m->next = NULL;
    if (ch->tail)
        ch->tail->next = m;
    else
        ch->head = m;
    ch->tail = m;
    ++ch->count;
}

static waiter *take_first(waiter **list) {
    waiter *w = *list;
    if (w)
        *list = w->next;
    return w;
}

static void add_last(waiter **list, waiter *w) {
    w->next = NULL;
    while (*list)
        list = &(*list)->next;
    *list = w;
}

// Only runs of the loop wait. Elsewhere a word that would wait ends the run with the fault
// PINAX_ERR_STATE: a host word that answers PINAX_PENDING without a token is one.
#define NOT_HERE PINAX_PENDING

static job *calling_job(pinax_call *call) {
    return current && current->state == pinax_call_state(call) ? current : NULL;
}

static pinax_err ch_send(pinax_call *call, void *user, const pinax_args *args) {
    pinax_channel *ch = user;
    if (args->s[0].len > ch->max_bytes)
        return PINAX_ERR_FULL;
    message *m = message_copy(ch->loop, args->s[0]);
    if (!m)
        return PINAX_ERR_FULL;
    lock(&ch->lock);
    if (ch->closed) {
        unlock(&ch->lock);
        loop_free(ch->loop, m);
        return PINAX_ERR_CANCELLED;
    }
    waiter *r = take_first(&ch->receivers);
    if (r) { // a receiver waits: the message goes straight to it
        wake(ch, r, PINAX_OK, m);
        unlock(&ch->lock);
        return PINAX_OK;
    }
    if (ch->count < ch->capacity) {
        append(ch, m);
        unlock(&ch->lock);
        return PINAX_OK;
    }
    job    *j = calling_job(call);
    waiter *w = j ? loop_alloc(ch->loop, sizeof *w) : NULL;
    if (!w) {
        unlock(&ch->lock);
        loop_free(ch->loop, m);
        return j ? PINAX_ERR_FULL : NOT_HERE;
    }
    *w = (waiter){.job = j, .token = pinax_call_pending(call), .msg = m};
    add_last(&ch->senders, w);
    unlock(&ch->lock);
    return PINAX_PENDING;
}

static pinax_err ch_receive(pinax_call *call, void *user, const pinax_args *args) {
    pinax_channel *ch = user;
    lock(&ch->lock);
    message *m = ch->head;
    if (m) {
        ch->head = m->next;
        if (!ch->head)
            ch->tail = NULL;
        --ch->count;
        waiter *s = take_first(&ch->senders);
        if (s) { // room again: the first waiting sender's message goes in, and the sender goes on
            append(ch, s->msg);
            s->msg = NULL;
            wake(ch, s, PINAX_OK, NULL);
        }
        unlock(&ch->lock);
        static const unsigned char nothing[1];
        pinax_mem room = m->len ? pinax_call_alloc(call, m->len) : (pinax_mem){(void *)nothing, 0};
        if (m->len && !room.ptr) {
            give_back(ch, m);
            return PINAX_ERR_FULL;
        }
        if (m->len)
            memcpy(room.ptr, m->bytes, m->len);
        args->rs[0] = (pinax_str){room.ptr, m->len};
        loop_free(ch->loop, m);
        return PINAX_OK;
    }
    if (ch->closed) {
        unlock(&ch->lock);
        return PINAX_ERR_NOT_FOUND;
    }
    job    *j = calling_job(call);
    waiter *w = j ? loop_alloc(ch->loop, sizeof *w) : NULL;
    if (!w) {
        unlock(&ch->lock);
        return j ? PINAX_ERR_FULL : NOT_HERE;
    }
    *w = (waiter){.job = j, .token = pinax_call_pending(call)};
    add_last(&ch->receivers, w);
    unlock(&ch->lock);
    return PINAX_PENDING;
}

void pinax_channel_close(pinax_channel *ch) {
    if (!ch)
        return;
    lock(&ch->lock);
    ch->closed = true;
    for (waiter *w; (w = take_first(&ch->receivers));)
        wake(ch, w, PINAX_ERR_NOT_FOUND, NULL);
    for (waiter *w; (w = take_first(&ch->senders));) {
        loop_free(ch->loop, w->msg);
        wake(ch, w, PINAX_ERR_CANCELLED, NULL);
    }
    unlock(&ch->lock);
}

static pinax_err ch_close(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)args;
    pinax_channel_close(user);
    return PINAX_OK;
}

// A message whose receiver stopped waiting before it got it: to the next receiver, or first in line.
static void give_back(pinax_channel *ch, message *m) {
    lock(&ch->lock);
    waiter *r = take_first(&ch->receivers);
    if (r) {
        wake(ch, r, PINAX_OK, m);
    } else {
        m->next = ch->head;
        ch->head = m;
        if (!ch->tail)
            ch->tail = m;
        ++ch->count;
    }
    unlock(&ch->lock);
}

// The run of `j` has ended: what it waited for in the channels is dropped.
static void forget(pinax_channel *ch, const job *j) {
    lock(&ch->lock);
    for (waiter **list = &ch->receivers; list; list = list == &ch->receivers ? &ch->senders : NULL) {
        for (waiter **at = list; *at;) {
            waiter *w = *at;
            if (w->job == j) {
                *at = w->next;
                loop_free(ch->loop, w->msg);
                loop_free(ch->loop, w);
            } else {
                at = &w->next;
            }
        }
    }
    unlock(&ch->lock);
}

static bool word_name(const char *s) {
    if (!*s || (*s >= '0' && *s <= '9'))
        return false;
    for (; *s; ++s) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
            return false;
    }
    return true;
}

pinax_err pinax_channel_open(pinax_loop *loop, const char *name, uint32_t messages, uint32_t message_bytes,
                             pinax_channel **out) {
    if (!out)
        return PINAX_ERR_ARGUMENT;
    *out = NULL;
    if (!loop || !name || !word_name(name) || strlen(name) >= sizeof((pinax_channel *)0)->name || !messages)
        return PINAX_ERR_ARGUMENT;
    pinax_channel *ch = loop_alloc(loop, sizeof *ch);
    if (!ch)
        return PINAX_ERR_NO_ROOM;
    if (!lock_init(&ch->lock)) {
        loop_free(loop, ch);
        return PINAX_ERR_NO_ROOM;
    }
    ch->loop = loop;
    memcpy(ch->name, name, strlen(name) + 1);
    ch->capacity = messages;
    ch->max_bytes = message_bytes;
    ch->defs[0] = (pinax_word_def){"send", "(msg str or fail)", ch_send, ch, PINAX_WORD_AWAITABLE};
    ch->defs[1] = (pinax_word_def){"receive", "(gives msg str or fail)", ch_receive, ch, PINAX_WORD_AWAITABLE};
    ch->defs[2] = (pinax_word_def){"close", "()", ch_close, ch, 0};
    lock(&loop->lock);
    ch->next = loop->channels;
    loop->channels = ch;
    unlock(&loop->lock);
    *out = ch;
    return PINAX_OK;
}

pinax_err pinax_channel_register(pinax_state *state, pinax_channel *ch) {
    if (!state || !ch)
        return PINAX_ERR_ARGUMENT;
    return pinax_register(state, ch->name, ch->defs, 3);
}

// ---- the loop -----------------------------------------------------------------------------

// The completions that came for a run, given to its state on the run's thread.
static void apply(const pinax_loop *loop, pinax_state *state, completion *c) {
    while (c) {
        completion      *next = c->next;
        pinax_str        text = c->msg ? (pinax_str){c->msg->bytes, c->msg->len} : (pinax_str){0};
        const pinax_args results = {.ri = c->ri, .rs = c->msg ? &text : c->rs};
        const pinax_err  err = pinax_complete(state, c->token, c->outcome, &results);
        if (c->msg && err == PINAX_ERR_NOT_FOUND) // the run that waited for it has gone
            give_back(c->from, c->msg);
        else
            loop_free(loop, c->msg);
        loop_free(loop, c);
        c = next;
    }
}

static void discard(const pinax_loop *loop, completion *c) {
    while (c) {
        completion *next = c->next;
        if (c->msg)
            give_back(c->from, c->msg);
        loop_free(loop, c);
        c = next;
    }
}

static void finish(pinax_loop *loop, job *j, pinax_err outcome) {
    // first out of the channels, so that no channel hands the run anything after its inbox is taken
    lock(&loop->lock);
    pinax_channel *channels = loop->channels;
    unlock(&loop->lock);
    for (pinax_channel *ch = channels; ch; ch = ch->next)
        forget(ch, j);
    lock(&loop->lock);
    for (job **at = &loop->active; *at; at = &(*at)->next_active) {
        if (*at == j) {
            *at = j->next_active;
            break;
        }
    }
    completion *left = j->inbox;
    j->inbox = NULL;
    unlock(&loop->lock);
    discard(loop, left);
    j->done(j->user, j->state, outcome);
    loop_free(loop, j);
    lock(&loop->lock);
    --loop->running;
    if (--loop->n_active == 0 && loop->waiting) {
        loop->waiting = false;
        (void)proven_sys_semaphore_post(&loop->idle);
    }
    check_deadlock(loop);
    unlock(&loop->lock);
}

static void run_job(pinax_loop *loop, job *j, completion *in) {
    if (j->deadlocked) {
        discard(loop, in);
        (void)pinax_abort(j->state);
        finish(loop, j, PINAX_ERR_DEADLOCK);
        return;
    }
    apply(loop, j->state, in);
    current = j;
    const pinax_err err = j->started ? pinax_resume(j->state, j->args) : pinax_run(j->state, j->word, j->args);
    current = NULL;
    j->started = true;
    if (err != PINAX_SUSPENDED && err != PINAX_WAITING) {
        finish(loop, j, err);
        return;
    }
    lock(&loop->lock);
    --loop->running;
    if (err == PINAX_SUSPENDED || j->inbox) {
        push_ready(loop, j);
    } else {
        j->kind = JOB_PARKED;
        check_deadlock(loop);
    }
    unlock(&loop->lock);
}

static void *worker(void *arg) {
    pinax_loop *loop = arg;
    for (;;) {
        while (!proven_sys_semaphore_wait(&loop->work)) {
        }
        lock(&loop->lock);
        job *j = loop->ready;
        if (!j) {
            const bool stop = loop->stopping;
            unlock(&loop->lock);
            if (stop)
                break;
            continue;
        }
        loop->ready = j->next_ready;
        if (!loop->ready)
            loop->ready_tail = &loop->ready;
        j->kind = JOB_RUNNING;
        ++loop->running;
        completion *in = j->inbox;
        j->inbox = NULL;
        j->inbox_tail = &j->inbox;
        unlock(&loop->lock);
        run_job(loop, j, in);
    }
    return NULL;
}

pinax_err pinax_loop_open(const pinax_loop_config *config, pinax_loop **out) {
    if (!out)
        return PINAX_ERR_ARGUMENT;
    *out = NULL;
    if (!config || config->struct_size != sizeof *config)
        return PINAX_ERR_ARGUMENT;
    const unsigned threads = config->threads;
    if (threads < 1 || threads > 256)
        return PINAX_ERR_ARGUMENT;
    pinax_loop_memory memory = config->memory;
    if (!memory.alloc && !memory.free) {
        memory = (pinax_loop_memory){NULL, heap_alloc, heap_free};
    } else if (!memory.alloc || !memory.free) { // one without the other: no way to give back, or to take
        return PINAX_ERR_ARGUMENT;
    }
    pinax_loop *loop = mem_alloc(&memory, sizeof *loop);
    if (!loop)
        return PINAX_ERR_NO_ROOM;
    loop->memory = memory;
    loop->threads = loop_alloc(loop, threads * sizeof *loop->threads);
    bool made_lock = false, made_work = false, made_idle = false;
    if (!loop->threads || !(made_lock = lock_init(&loop->lock)) ||
        !(made_work = proven_sys_semaphore_init(&loop->work)) || !(made_idle = proven_sys_semaphore_init(&loop->idle))) {
        if (made_lock)
            proven_sys_semaphore_destroy(&loop->lock);
        if (made_work)
            proven_sys_semaphore_destroy(&loop->work);
        loop_free(loop, loop->threads);
        mem_free(&memory, loop);
        return PINAX_ERR_NO_ROOM;
    }
    loop->ready_tail = &loop->ready;
    for (; loop->n_threads < threads; ++loop->n_threads) {
        loop->threads[loop->n_threads] = proven_sys_thread_create(worker, loop);
        if (!loop->threads[loop->n_threads].internal) {
            pinax_loop_close(loop);
            return PINAX_ERR_NO_ROOM;
        }
    }
    *out = loop;
    return PINAX_OK;
}

pinax_err pinax_loop_start(pinax_loop *loop, pinax_state *state, pinax_word word, const pinax_args *args,
                           pinax_loop_done done, void *user) {
    if (!loop || !state || !args || !done)
        return PINAX_ERR_ARGUMENT;
    job *j = loop_alloc(loop, sizeof *j);
    if (!j)
        return PINAX_ERR_NO_ROOM;
    *j = (job){.state = state, .word = word, .args = args, .done = done, .user = user};
    j->inbox_tail = &j->inbox;
    lock(&loop->lock);
    if (loop->stopping || find_job(loop, state)) {
        unlock(&loop->lock);
        loop_free(loop, j);
        return PINAX_ERR_STATE;
    }
    j->next_active = loop->active;
    loop->active = j;
    ++loop->n_active;
    push_ready(loop, j);
    unlock(&loop->lock);
    return PINAX_OK;
}

void pinax_loop_wait(pinax_loop *loop) {
    if (!loop)
        return;
    lock(&loop->lock);
    if (loop->n_active == 0) {
        unlock(&loop->lock);
        return;
    }
    loop->waiting = true;
    check_deadlock(loop);
    unlock(&loop->lock);
    while (!proven_sys_semaphore_wait(&loop->idle)) {
    }
}

void pinax_loop_close(pinax_loop *loop) {
    if (!loop)
        return;
    lock(&loop->lock);
    loop->stopping = true;
    unlock(&loop->lock);
    for (unsigned k = 0; k < loop->n_threads; ++k)
        (void)proven_sys_semaphore_post(&loop->work);
    for (unsigned k = 0; k < loop->n_threads; ++k)
        proven_sys_thread_join(loop->threads[k]);
    for (pinax_channel *ch = loop->channels; ch;) {
        pinax_channel *next = ch->next;
        for (message *m = ch->head; m;) {
            message *after = m->next;
            loop_free(loop, m);
            m = after;
        }
        for (waiter **list = &ch->receivers; list; list = list == &ch->receivers ? &ch->senders : NULL) {
            for (waiter *w = *list; w;) {
                waiter *after = w->next;
                loop_free(loop, w->msg);
                loop_free(loop, w);
                w = after;
            }
        }
        proven_sys_semaphore_destroy(&ch->lock);
        loop_free(loop, ch);
        ch = next;
    }
    proven_sys_semaphore_destroy(&loop->idle);
    proven_sys_semaphore_destroy(&loop->work);
    proven_sys_semaphore_destroy(&loop->lock);
    loop_free(loop, loop->threads);
    const pinax_loop_memory memory = loop->memory; // the loop goes back through its own functions, last
    mem_free(&memory, loop);
}

pinax_token pinax_loop_pending(pinax_loop *loop, pinax_call *call) {
    lock(&loop->lock);
    ++loop->outstanding;
    unlock(&loop->lock);
    return pinax_call_pending(call);
}

pinax_err pinax_loop_complete(pinax_loop *loop, pinax_state *state, pinax_token token, pinax_err outcome,
                              const int64_t *ri, size_t ni, const pinax_str *rs, size_t ns) {
    if (!loop || !state || (ni && !ri) || (ns && !rs) || ni > 64 || ns > 64)
        return PINAX_ERR_ARGUMENT;
    size_t bytes = sizeof(completion) + ni * sizeof(int64_t) + ns * sizeof(pinax_str);
    for (size_t k = 0; k < ns; ++k)
        bytes += rs[k].len;
    completion *c = loop_alloc(loop, bytes);
    if (!c)
        return PINAX_ERR_NO_ROOM;
    c->token = token, c->outcome = outcome, c->ni = ni, c->ns = ns;
    c->ri = (int64_t *)(void *)(c + 1);
    c->rs = (pinax_str *)(void *)(c->ri + ni);
    unsigned char *text = (unsigned char *)(c->rs + ns);
    if (ni)
        memcpy(c->ri, ri, ni * sizeof *ri);
    for (size_t k = 0; k < ns; ++k) {
        if (rs[k].len)
            memcpy(text, rs[k].ptr, rs[k].len);
        c->rs[k] = (pinax_str){text, rs[k].len};
        text += rs[k].len;
    }
    lock(&loop->lock);
    if (loop->outstanding)
        --loop->outstanding;
    job *j = find_job(loop, state);
    if (!j) {
        check_deadlock(loop);
        unlock(&loop->lock);
        loop_free(loop, c);
        return PINAX_ERR_NOT_FOUND;
    }
    c->next = NULL;
    *j->inbox_tail = c;
    j->inbox_tail = &c->next;
    if (j->kind == JOB_PARKED)
        push_ready(loop, j);
    unlock(&loop->lock);
    return PINAX_OK;
}

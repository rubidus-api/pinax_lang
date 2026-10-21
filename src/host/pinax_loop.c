// pinax_loop.c - the default host loop and channels (see pinax_loop.h). Written against pinax.h;
// threads and their semaphores come from the vendored proven.
//
// Memory: one block, the host's or one piece from proven's heap, and nothing else. The loop's
// objects live in fixed slots of it: the runs it is given (jobs), the completions that wake a
// parked run (one per run, so that slab cannot run out) and the completions a host operation
// brings (as many; one more is NO_ROOM). A channel carves its own slots from what is left of
// the block when it is opened: its waiting runs (one per run) and its messages (its capacity,
// plus one per run that waits to send, plus one per run that died with a message in flight).
// A slot is taken and given back in place through a free list threaded through the free slots.
//
// Locks: each channel has one, the loop has one. A channel's lock may be held while the loop's is
// taken (to hand a run its completion); never the other way round. A slab is touched only under
// its owner's lock: jobs and completions under the loop's, a channel's waiters and messages under
// that channel's.
#include "pinax_loop.h"

#include "../../vendor/proven/platform/proven_sys_thread.h"
#include "proven/heap.h"

#include <string.h>

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

// ---- slots ------------------------------------------------------------------------------

enum { SLOT_ALIGN = 16 };

static size_t round_up(size_t bytes) {
    return (bytes + SLOT_ALIGN - 1) & ~(size_t)(SLOT_ALIGN - 1);
}

// `count` slots of `item` bytes each; the free ones are a list through their first word.
typedef struct slab {
    unsigned char *base;
    size_t         item;
    uint32_t       count;
    void          *free_list;
} slab;

static void slab_init(slab *s, void *base, size_t item, uint32_t count) {
    s->base = base;
    s->item = item;
    s->count = count;
    s->free_list = NULL;
    for (uint32_t k = count; k-- > 0;) {
        void *slot = s->base + (size_t)k * item;
        memcpy(slot, &s->free_list, sizeof s->free_list);
        s->free_list = slot;
    }
}

// A free slot with its first `clear` bytes zeroed, or NULL when none is free.
static void *slab_take(slab *s, size_t clear) {
    void *slot = s->free_list;
    if (slot) {
        memcpy(&s->free_list, slot, sizeof s->free_list);
        memset(slot, 0, clear);
    }
    return slot;
}

static void slab_give(slab *s, void *slot) {
    if (slot) {
        memcpy(slot, &s->free_list, sizeof s->free_list);
        s->free_list = slot;
    }
}

// ---- objects ----------------------------------------------------------------------------

typedef struct message {
    struct message *next;
    size_t          len;
    unsigned char   bytes[];
} message;

typedef struct job job;

// A run parked on a channel: a receiver, or a sender with its message, waiting for room.
typedef struct waiter {
    struct waiter *next;
    job           *job;
    pinax_token    token;
    message       *msg;
} waiter;

// What finishes one operation a run waits for, kept until the run's thread takes it. A wake
// (from a channel) carries a message at most; a host operation's results follow the struct.
typedef struct completion {
    struct completion *next;
    pinax_token        token;
    pinax_err          outcome;
    bool               host; // which slab it came from
    pinax_channel     *from; // a message handed over by this channel: given back when nobody waits for it
    message           *msg;
    size_t             ni, ns;
    int64_t           *ri; // in the same slot, as are the strings' bytes
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
    slab           waiters, messages;
    void          *own; // its piece of the heap, when the loop has no block
};

struct pinax_loop {
    lock_t                 lock;
    proven_sys_semaphore_t work, idle;
    proven_sys_thread_t   *threads;
    unsigned               n_threads;
    uint32_t               states, result_bytes;
    job                   *ready, **ready_tail, *active;
    size_t                 n_active, running, outstanding;
    bool                   stopping, waiting;
    pinax_channel         *channels;
    slab                   jobs, wakes, hosts;
    unsigned char         *block; // what is left of the host's block, for channels
    size_t                 block_len;
    bool                   heap;  // no block: the loop and each channel are pieces of the heap
};

static thread_local job *current; // the run this thread is in, while it is in one

// ---- the layout of the block --------------------------------------------------------------

static bool config_ok(const pinax_loop_config *config, pinax_loop_config *filled) {
    if (!config || config->struct_size != sizeof *config || config->threads < 1 || config->threads > 256)
        return false;
    *filled = *config;
    if (!filled->states)
        filled->states = 16;
    if (!filled->result_bytes)
        filled->result_bytes = 4096;
    return true;
}

// The loop's part: the struct, the threads, the jobs, the wake completions, the host completions.
typedef struct layout {
    size_t threads, jobs, wakes, hosts, total;
} layout;

static layout layout_of(const pinax_loop_config *c) {
    layout l;
    size_t at = round_up(sizeof(pinax_loop));
    l.threads = at, at += round_up((size_t)c->threads * sizeof(proven_sys_thread_t));
    l.jobs = at, at += (size_t)c->states * round_up(sizeof(job));
    l.wakes = at, at += (size_t)c->states * round_up(sizeof(completion));
    l.hosts = at, at += (size_t)c->states * round_up(sizeof(completion) + c->result_bytes);
    l.total = at + SLOT_ALIGN; // a host's block may be unaligned by up to that
    return l;
}

size_t pinax_loop_size(const pinax_loop_config *config) {
    pinax_loop_config c;
    if (!config_ok(config, &c))
        return 0;
    return layout_of(&c).total;
}

// A channel's part: the struct, one waiter per run, and its messages.
static size_t message_slots(uint32_t states, uint32_t messages) {
    return (size_t)messages + 2u * states;
}

static size_t channel_bytes(uint32_t states, uint32_t messages, uint32_t message_bytes) {
    return round_up(sizeof(pinax_channel)) + (size_t)states * round_up(sizeof(waiter)) +
           message_slots(states, messages) * round_up(sizeof(message) + message_bytes) + SLOT_ALIGN;
}

size_t pinax_channel_size(const pinax_loop_config *config, uint32_t messages, uint32_t message_bytes) {
    pinax_loop_config c;
    if (!config_ok(config, &c) || !messages)
        return 0;
    return channel_bytes(c.states, messages, message_bytes);
}

static unsigned char *aligned(void *p) {
    return (unsigned char *)(((uintptr_t)p + SLOT_ALIGN - 1) & ~(uintptr_t)(SLOT_ALIGN - 1));
}

static void heap_free(void *object) {
    if (object) {
        const proven_allocator_t heap = proven_heap_allocator();
        heap.free_fn(heap.ctx, object);
    }
}

static void *heap_take(size_t bytes) {
    const proven_allocator_t heap = proven_heap_allocator();
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, bytes, SLOT_ALIGN);
    return room.err == PROVEN_OK ? room.value.ptr : NULL;
}

// ---- the ready line and the completions ---------------------------------------------------

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

static void deliver(pinax_loop *loop, job *j, completion *c) { // loop locked
    c->next = NULL;
    *j->inbox_tail = c;
    j->inbox_tail = &c->next;
    if (j->kind == JOB_PARKED)
        push_ready(loop, j);
}

// Wakes the run of `j` with an outcome and, for a receiver, its message. One wake completion per
// run is in the slab, and a parked run holds one wait: it is never missing.
static void post(pinax_loop *loop, job *j, pinax_token token, pinax_err outcome, pinax_channel *from, message *m) {
    lock(&loop->lock);
    completion *c = slab_take(&loop->wakes, sizeof *c);
    c->token = token, c->outcome = outcome;
    c->msg = m;
    c->from = m ? from : NULL;
    deliver(loop, j, c);
    unlock(&loop->lock);
}

static void completion_give(pinax_loop *loop, completion *c) {
    lock(&loop->lock);
    slab_give(c->host ? &loop->hosts : &loop->wakes, c);
    unlock(&loop->lock);
}

// ---- channels -----------------------------------------------------------------------------

static void message_give(pinax_channel *ch, message *m) { // channel locked
    slab_give(&ch->messages, m);
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

static void prepend(pinax_channel *ch, message *m) { // channel locked
    m->next = ch->head;
    ch->head = m;
    if (!ch->tail)
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

// Wakes a parked run and gives its waiter back. Channel locked.
static void wake(pinax_channel *ch, waiter *w, pinax_err outcome, message *m) {
    post(ch->loop, w->job, w->token, outcome, ch, m);
    slab_give(&ch->waiters, w);
}

// Only runs of the loop wait. Elsewhere a word that would wait ends the run with the fault
// PINAX_ERR_STATE: a host word that answers PINAX_PENDING without a token is one.
#define NOT_HERE PINAX_PENDING

static job *calling_job(pinax_call *call) {
    return current && current->state == pinax_call_state(call) ? current : NULL;
}

static pinax_err ch_send(pinax_call *call, void *user, const pinax_args *args) {
    pinax_channel *ch = user;
    const pinax_str text = args->s[0];
    if (text.len > ch->max_bytes)
        return PINAX_ERR_FULL;
    lock(&ch->lock);
    if (ch->closed) {
        unlock(&ch->lock);
        return PINAX_ERR_CANCELLED;
    }
    message *m = slab_take(&ch->messages, sizeof *m);
    if (!m) { // every slot holds a message: as full as a channel gets
        unlock(&ch->lock);
        return PINAX_ERR_FULL;
    }
    m->len = text.len;
    if (text.len)
        memcpy(m->bytes, text.ptr, text.len);
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
    waiter *w = j ? slab_take(&ch->waiters, sizeof *w) : NULL;
    if (!w) {
        message_give(ch, m);
        unlock(&ch->lock);
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
        static const unsigned char nothing[1];
        pinax_mem room = m->len ? pinax_call_alloc(call, m->len) : (pinax_mem){(void *)nothing, 0};
        if (m->len && !room.ptr) { // the run has no room for it: back in front, for the next receive
            prepend(ch, m);
            unlock(&ch->lock);
            return PINAX_ERR_FULL;
        }
        if (m->len)
            memcpy(room.ptr, m->bytes, m->len);
        args->rs[0] = (pinax_str){room.ptr, m->len};
        message_give(ch, m);
        unlock(&ch->lock);
        return PINAX_OK;
    }
    if (ch->closed) {
        unlock(&ch->lock);
        return PINAX_ERR_NOT_FOUND;
    }
    job    *j = calling_job(call);
    waiter *w = j ? slab_take(&ch->waiters, sizeof *w) : NULL;
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
        message_give(ch, w->msg);
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
    if (r)
        wake(ch, r, PINAX_OK, m);
    else
        prepend(ch, m);
    unlock(&ch->lock);
}

// A message nobody will take any more.
static void drop(pinax_channel *ch, message *m) {
    lock(&ch->lock);
    message_give(ch, m);
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
                message_give(ch, w->msg);
                slab_give(&ch->waiters, w);
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
    const size_t bytes = channel_bytes(loop->states, messages, message_bytes);
    unsigned char *piece;
    void          *own = NULL;
    if (loop->heap) {
        own = heap_take(bytes);
        if (!own)
            return PINAX_ERR_NO_ROOM;
        piece = own;
    } else { // carved from what is left of the block, under the loop's lock: channels open from any thread
        lock(&loop->lock);
        unsigned char *at = aligned(loop->block);
        const size_t   skip = (size_t)(at - loop->block);
        if (skip + bytes > loop->block_len) {
            unlock(&loop->lock);
            return PINAX_ERR_NO_ROOM;
        }
        loop->block += skip + bytes;
        loop->block_len -= skip + bytes;
        unlock(&loop->lock);
        piece = at;
    }
    pinax_channel *ch = (pinax_channel *)(void *)aligned(piece);
    memset(ch, 0, sizeof *ch);
    if (!lock_init(&ch->lock)) {
        heap_free(own);
        return PINAX_ERR_NO_ROOM;
    }
    unsigned char *at = (unsigned char *)ch + round_up(sizeof *ch);
    slab_init(&ch->waiters, at, round_up(sizeof(waiter)), loop->states);
    at += (size_t)loop->states * round_up(sizeof(waiter));
    slab_init(&ch->messages, at, round_up(sizeof(message) + message_bytes), (uint32_t)message_slots(loop->states, messages));
    ch->own = own;
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
static void apply(pinax_loop *loop, pinax_state *state, completion *c) {
    while (c) {
        completion      *next = c->next;
        pinax_str        text = c->msg ? (pinax_str){c->msg->bytes, c->msg->len} : (pinax_str){0};
        const pinax_args results = {.ri = c->ri, .rs = c->msg ? &text : c->rs};
        const pinax_err  err = pinax_complete(state, c->token, c->outcome, &results);
        if (c->msg && err == PINAX_ERR_NOT_FOUND) // the run that waited for it has gone
            give_back(c->from, c->msg);
        else if (c->msg)
            drop(c->from, c->msg);
        completion_give(loop, c);
        c = next;
    }
}

static void discard(pinax_loop *loop, completion *c) {
    while (c) {
        completion *next = c->next;
        if (c->msg)
            give_back(c->from, c->msg);
        completion_give(loop, c);
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
    // the slot goes back before done() runs, so that a run started from done() can have it
    const pinax_loop_done done = j->done;
    void *const           user = j->user;
    pinax_state *const    state = j->state;
    lock(&loop->lock);
    slab_give(&loop->jobs, j);
    unlock(&loop->lock);
    done(user, state, outcome);
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
    pinax_loop_config c;
    if (!config_ok(config, &c))
        return PINAX_ERR_ARGUMENT;
    const layout   l = layout_of(&c);
    unsigned char *piece = c.block.ptr;
    size_t         piece_len = c.block.len;
    const bool     heap = !piece;
    if (heap) {
        piece = heap_take(l.total);
        if (!piece)
            return PINAX_ERR_NO_ROOM;
        piece_len = l.total;
    }
    unsigned char *at = aligned(piece);
    const size_t   skip = (size_t)(at - piece);
    if (skip + l.total - SLOT_ALIGN > piece_len) { // the alignment slack is what the sizes include
        if (heap)
            heap_free(piece);
        return PINAX_ERR_NO_ROOM;
    }
    pinax_loop *loop = (pinax_loop *)(void *)at;
    memset(loop, 0, sizeof *loop);
    loop->heap = heap;
    loop->states = c.states;
    loop->result_bytes = c.result_bytes;
    loop->threads = (proven_sys_thread_t *)(void *)(at + l.threads);
    slab_init(&loop->jobs, at + l.jobs, round_up(sizeof(job)), c.states);
    slab_init(&loop->wakes, at + l.wakes, round_up(sizeof(completion)), c.states);
    slab_init(&loop->hosts, at + l.hosts, round_up(sizeof(completion) + c.result_bytes), c.states);
    loop->block = heap ? NULL : at + l.total - SLOT_ALIGN;
    loop->block_len = heap ? 0 : piece_len - skip - (l.total - SLOT_ALIGN);
    bool made_lock = false, made_work = false, made_idle = false;
    if (!(made_lock = lock_init(&loop->lock)) || !(made_work = proven_sys_semaphore_init(&loop->work)) ||
        !(made_idle = proven_sys_semaphore_init(&loop->idle))) {
        if (made_lock)
            proven_sys_semaphore_destroy(&loop->lock);
        if (made_work)
            proven_sys_semaphore_destroy(&loop->work);
        if (heap)
            heap_free(piece);
        return PINAX_ERR_NO_ROOM;
    }
    loop->ready_tail = &loop->ready;
    for (; loop->n_threads < c.threads; ++loop->n_threads) {
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
    lock(&loop->lock);
    if (loop->stopping || find_job(loop, state)) {
        unlock(&loop->lock);
        return PINAX_ERR_STATE;
    }
    job *j = slab_take(&loop->jobs, sizeof *j);
    if (!j) {
        unlock(&loop->lock);
        return PINAX_ERR_NO_ROOM;
    }
    *j = (job){.state = state, .word = word, .args = args, .done = done, .user = user};
    j->inbox_tail = &j->inbox;
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
    // the threads are gone: no lock is needed, and the slots need no giving back
    for (pinax_channel *ch = loop->channels; ch;) {
        pinax_channel *next = ch->next;
        proven_sys_semaphore_destroy(&ch->lock);
        heap_free(ch->own);
        ch = next;
    }
    proven_sys_semaphore_destroy(&loop->idle);
    proven_sys_semaphore_destroy(&loop->work);
    proven_sys_semaphore_destroy(&loop->lock);
    if (loop->heap)
        heap_free(loop); // the loop is at the front of its piece, which was taken aligned
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
    size_t bytes = ni * sizeof(int64_t) + ns * sizeof(pinax_str);
    for (size_t k = 0; k < ns; ++k)
        bytes += rs[k].len;
    if (bytes > loop->result_bytes)
        return PINAX_ERR_NO_ROOM;
    lock(&loop->lock);
    if (loop->outstanding)
        --loop->outstanding;
    job *j = find_job(loop, state);
    if (!j) {
        check_deadlock(loop);
        unlock(&loop->lock);
        return PINAX_ERR_NOT_FOUND;
    }
    completion *c = slab_take(&loop->hosts, sizeof *c);
    if (!c) {
        unlock(&loop->lock);
        return PINAX_ERR_NO_ROOM;
    }
    c->host = true;
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
    deliver(loop, j, c);
    unlock(&loop->lock);
    return PINAX_OK;
}

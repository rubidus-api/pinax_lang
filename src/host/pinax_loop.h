/*
 * pinax_loop.h - a default host loop for Pinax: states run on a few threads, and channels carry
 * messages between them (RFC-0005 section 4, RFC-0020). Written against pinax.h alone, in the
 * host library; it uses the operating system's threads. Its memory is one block the host gives,
 * as a state's is: the loop's objects (the runs it is given, their completions) are fixed slots
 * in it, taken and given back in place, and each channel carves its slots (messages, waiting
 * runs) from the same block when it is opened. Nothing is allocated while runs go on.
 *
 * The loop runs words: pinax_loop_start() hands it a state and a word, and one of its threads
 * calls pinax_run() and pinax_resume() until the run ends. A run that runs out of its budget goes
 * to the back of the line, so the budget is the time slice between states (a state with no budget
 * keeps its thread until it ends or waits). A run that waits (PINAX_WAITING) gives its thread back
 * until what it waits for is completed. A state belongs to the loop from pinax_loop_start() until
 * its done() has been called; the host does not use it meanwhile.
 *
 * A channel is a bounded line of byte messages. pinax_channel_register() makes it a module of a
 * state, under the channel's name, with three words:
 *
 *   send    (msg str or fail)        waits while the channel is full
 *   receive (gives msg str or fail)  waits while it is empty
 *   close   ()                       no more messages
 *
 * A message is copied in and out; scripts pack a row with bytes.pack and read it back with
 * bytes.unpack. send fails with full when the message is longer than the channel allows and with
 * cancelled once the channel is closed; receive fails with not_found once it is closed and empty.
 * Only runs the loop started wait: elsewhere a send or receive that would wait is the fault
 * PINAX_ERR_STATE.
 *
 * While the host waits in pinax_loop_wait(), when every run the loop has left waits for a channel
 * and no host operation is outstanding (see pinax_loop_pending), nothing can wake them: the loop
 * ends them all with PINAX_ERR_DEADLOCK. Before the host waits it may still start the run that
 * sends, so nothing is ended then.
 */
#ifndef PINAX_LOOP_H
#define PINAX_LOOP_H

#include "pinax.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pinax_loop    pinax_loop;
typedef struct pinax_channel pinax_channel;

/* How a run ended: PINAX_OK, a fault, or PINAX_ERR_DEADLOCK. Called on one of the loop's threads;
 * the state is the host's again (it may start another run on it here). */
typedef void (*pinax_loop_done)(void *user, pinax_state *state, pinax_err outcome);

/* Capacities, and the block. Zero means "default"; struct_size lets the struct grow compatibly.
 *   threads      1 to 256, no default
 *   states       runs the loop holds at once (started and not yet ended), default 16; one more
 *                is PINAX_ERR_NO_ROOM from pinax_loop_start
 *   result_bytes the most a host operation's results take in a completion (8 per integer, 16
 *                per string, plus the strings' bytes), default 4096; more is PINAX_ERR_NO_ROOM
 *                from pinax_loop_complete
 *   block        memory for the loop and its channels: pinax_loop_size() bytes for the loop,
 *                plus pinax_channel_size() for each channel opened on it. With block.ptr NULL the
 *                loop takes one piece of that size from proven's heap, and one per channel, and
 *                gives them back in pinax_loop_close(). */
typedef struct pinax_loop_config {
    size_t    struct_size; /* sizeof(pinax_loop_config) */
    unsigned  threads;
    uint32_t  states;
    uint32_t  result_bytes;
    pinax_mem block;
} pinax_loop_config;

/* The bytes of the block the loop itself takes for this config (0 for a config it refuses), and
 * the bytes a channel of `messages` messages of up to `message_bytes` each takes on a loop of
 * this config, both enough for a block of any alignment. The sum over the loop and its channels
 * is the block. */
size_t pinax_loop_size(const pinax_loop_config *config);
size_t pinax_channel_size(const pinax_loop_config *config, uint32_t messages, uint32_t message_bytes);

/* A loop with config->threads threads. PINAX_ERR_ARGUMENT for a config it does not understand
 * or a thread count outside 1 to 256; PINAX_ERR_NO_ROOM when the block is too small for the
 * loop, or memory or a thread cannot be had. */
pinax_err pinax_loop_open(const pinax_loop_config *config, pinax_loop **out);
/* Runs `word` of `state` with `args` until it ends, then calls done(user, state, outcome). args
 * and what it points to (inputs and the places of the results) stay the host's and must live
 * until then. PINAX_ERR_STATE when the state already runs on the loop or the loop is closing. */
pinax_err pinax_loop_start(pinax_loop *loop, pinax_state *state, pinax_word word, const pinax_args *args,
                           pinax_loop_done done, void *user);
/* Waits until every run the loop was given has ended (runs started from done() included). */
void      pinax_loop_wait(pinax_loop *loop);
/* After pinax_loop_wait(): stops the threads and frees the loop and its channels. */
void      pinax_loop_close(pinax_loop *loop);

/* For a host's own awaitable word on a state the loop runs: use this in place of
 * pinax_call_pending(), so that the loop knows an operation is outstanding, and finish it with
 * pinax_loop_complete() from any thread. ni integer and ns string results, from index 0;
 * strings are copied before this returns. */
pinax_token pinax_loop_pending(pinax_loop *loop, pinax_call *call);
pinax_err   pinax_loop_complete(pinax_loop *loop, pinax_state *state, pinax_token token, pinax_err outcome,
                                const int64_t *ri, size_t ni, const pinax_str *rs, size_t ns);

/* A channel named `name` (written like a word) holding at most `messages` messages of at most
 * `message_bytes` bytes each. It lives until the loop is closed. Its slots are carved from the
 * loop's block: PINAX_ERR_NO_ROOM when pinax_channel_size() bytes are not left there. */
pinax_err pinax_channel_open(pinax_loop *loop, const char *name, uint32_t messages, uint32_t message_bytes,
                             pinax_channel **out);
/* Makes the channel the module `name` of the state; before the scripts that use it load, and
 * again after pinax_image_open() or pinax_clone(). */
pinax_err pinax_channel_register(pinax_state *state, pinax_channel *channel);
/* As a script's close: from any thread. */
void      pinax_channel_close(pinax_channel *channel);

#ifdef __cplusplus
}
#endif

#endif /* PINAX_LOOP_H */

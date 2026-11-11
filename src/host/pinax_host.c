// pinax_host.c - the standard host modules (see pinax_host.h). Written against pinax.h; the
// operating system is reached through the vendored proven.
#if !defined(_WIN32) && !defined(_WIN64) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L // popen, pclose, setenv, chdir: the sh module
#endif
#include "pinax_host.h"
#include "pnx_streams.h"

#include "pinax_proven.h"

#include "proven/fs.h"
#include "proven/heap.h"
#include "proven/mmap.h"
#include "proven/random.h"
#include "proven/sysio.h"
#include "proven/time.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static proven_u8str_view_t view_of(pinax_str s) {
    return (proven_u8str_view_t){s.ptr, s.len};
}

static pinax_err from(proven_err_t err) {
    return err == PROVEN_OK ? PINAX_OK : pinax_err_from_proven(err);
}

// ---- io -----------------------------------------------------------------------------------

static pinax_err write_to(proven_file_t file, pinax_str text, bool newline) {
    // a host word without `or fail` must not fail: a closed output is ignored, as printf does
    (void)proven_fs_write_all(file, (proven_mem_view_t){text.ptr, text.len});
    if (newline)
        (void)proven_fs_write_all(file, (proven_mem_view_t){(const proven_byte_t *)"\n", 1});
    return PINAX_OK;
}
// pnx's own standard streams (pnx_streams.h): standard output kept in a buffer, standard input read
// ahead. Off unless pnx turns it on; the process has one of each, and pnx runs one thread.
static struct {
    bool          out_kept, in_ahead;
    size_t        out_n;
    unsigned char out[65536];
    unsigned char in[4096];
} streams;

void pnx_streams_own(bool read_ahead) {
    streams.out_kept = true;
    streams.in_ahead = read_ahead;
}

void pnx_streams_flush(void) {
    if (streams.out_n)
        (void)proven_fs_write_all(proven_sysio_stdout(), (proven_mem_view_t){streams.out, streams.out_n});
    streams.out_n = 0;
}

static void keep_out(const unsigned char *bytes, size_t n) {
    if (streams.out_n + n > sizeof streams.out)
        pnx_streams_flush();
    if (n > sizeof streams.out) {
        (void)proven_fs_write_all(proven_sysio_stdout(), (proven_mem_view_t){bytes, n});
        return;
    }
    memcpy(streams.out + streams.out_n, bytes, n);
    streams.out_n += n;
}

static pinax_err to_stdout(pinax_str text, bool newline) {
    if (!streams.out_kept)
        return write_to(proven_sysio_stdout(), text, newline);
    keep_out(text.ptr, text.len);
    if (newline)
        keep_out((const unsigned char *)"\n", 1);
    return PINAX_OK;
}

static pinax_err io_print(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    return to_stdout(args->s[0], false);
}
static pinax_err io_println(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    return to_stdout(args->s[0], true);
}
static pinax_err io_eprint(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    pnx_streams_flush(); // what was written before it comes first
    return write_to(proven_sysio_stderr(), args->s[0], false);
}

// Where lines are read from: a file and the bytes read ahead of the script, ahead[at..end).
// Standard input is read one byte at a time (size 1), so that a host that reads it too finds
// the bytes after the line still there; a file of the module reads ahead (T083). `show`: the
// output kept back is written before each read of the system, so that a prompt shows before
// the wait, and a line answered at once costs no write of its own (T090).
typedef struct {
    proven_file_t  file;
    unsigned char *ahead;
    size_t         at, end, size;
    bool           show;
} reader;

// The next byte; false at the end of the input, and with *err set when the system refused.
static bool next_byte(reader *r, unsigned char *byte, proven_err_t *err) {
    if (r->at == r->end) {
        if (r->show)
            pnx_streams_flush();
        const proven_result_size_t got = proven_fs_read(r->file, (proven_mem_mut_t){r->ahead, r->size});
        if (got.err != PROVEN_OK && got.err != PROVEN_ERR_EOF) {
            *err = got.err;
            return false;
        }
        if (got.err == PROVEN_ERR_EOF || got.value == 0)
            return false;
        r->at = 0;
        r->end = got.value;
    }
    *byte = r->ahead[r->at++];
    return true;
}

// Reads one line (without its line break) into the caller's region. Fails with not_found at
// the end of the input and with full when the line does not fit.
static pinax_err line_from(pinax_call *call, reader *in, pinax_str *out) {
    enum { MOST = 4096 };
    pinax_mem room = pinax_call_alloc(call, MOST);
    if (!room.ptr)
        return PINAX_ERR_FULL;
    unsigned char *line = room.ptr;
    size_t n = 0;
    for (;;) {
        unsigned char byte;
        proven_err_t  err = PROVEN_OK;
        if (!next_byte(in, &byte, &err)) {
            if (err != PROVEN_OK)
                return from(err);
            if (n == 0) // the end: a last line may lack its line break
                return PINAX_ERR_NOT_FOUND;
            break;
        }
        if (byte == '\n')
            break;
        if (n == MOST)
            return PINAX_ERR_FULL;
        line[n++] = byte;
    }
    if (n && line[n - 1] == '\r')
        --n;
    out->ptr = line;
    out->len = n;
    return PINAX_OK;
}
// Standard input: one byte at a time, or for pnx through its read-ahead. Output kept back is written
// before the system is asked for more (`show`), so that a prompt shows before the line is read.
static reader stdin_ahead;

static reader *stdin_reader(reader *one_byte, unsigned char *one) {
    if (!streams.in_ahead) {
        *one_byte = (reader){proven_sysio_stdin(), one, 0, 0, 1, true};
        return one_byte;
    }
    if (!stdin_ahead.ahead)
        stdin_ahead = (reader){proven_sysio_stdin(), streams.in, 0, 0, sizeof streams.in, true};
    return &stdin_ahead;
}

static pinax_err io_read_line(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    unsigned char one;
    reader        in;
    return line_from(call, stdin_reader(&in, &one), &args->rs[0]);
}

// The buffer forms (RFC-0009 section 5): a line is appended to a buffer the script declared,
// without its line break. At the end of the input the word fails with not_found; a line that
// does not fit fails with full, and what fitted stays in the buffer.
static pinax_err line_into(pinax_call *call, reader *in, int64_t buffer) {
    const pinax_mem room = pinax_call_buffer(call, buffer, 0);
    unsigned char  *line = room.ptr;
    size_t          n = 0;
    bool            any = false;
    for (;;) {
        unsigned char byte;
        proven_err_t  err = PROVEN_OK;
        if (!next_byte(in, &byte, &err)) {
            if (err != PROVEN_OK)
                return from(err);
            if (!any)
                return PINAX_ERR_NOT_FOUND;
            break;
        }
        any = true;
        if (byte == '\n')
            break;
        if (n == room.len) {
            (void)pinax_call_buffer(call, buffer, n);
            return PINAX_ERR_FULL;
        }
        line[n++] = byte;
    }
    if (n && line[n - 1] == '\r')
        --n;
    (void)pinax_call_buffer(call, buffer, n);
    return PINAX_OK;
}
static pinax_err io_read_line_into(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    unsigned char one;
    reader        in;
    return line_into(call, stdin_reader(&in, &one), args->i[0]);
}

// ---- file ---------------------------------------------------------------------------------

// The handle type number of "file" in this state travels in the `user` pointer of the words.
static uint32_t file_type(void *user) {
    return (uint32_t)(uintptr_t)user;
}

// An open file of the module: what proven opened, and the bytes its reads took ahead. A pipe
// to or from a command (sh.open_read, sh.open_write) is a file too, read and written through
// its descriptor; `pipe` is what popen gave, closed with pclose, which waits for the command.
typedef struct {
    reader        in;
    FILE         *pipe;
    unsigned char bytes[4096];
} host_file;

static void sh_ended(int raw);

static void file_close_object(void *object, void *user) {
    (void)user;
    host_file *file = object;
    if (file->pipe)
        sh_ended(pclose(file->pipe));
    else
        (void)proven_fs_close(file->in.file);
    proven_allocator_t heap = proven_heap_allocator();
    heap.free_fn(heap.ctx, file);
}

static pinax_err file_open(pinax_call *call, void *user, const pinax_args *args, proven_fs_mode_t mode) {
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_file_t opened = proven_fs_open(heap, view_of(args->s[0]), mode);
    if (opened.err != PROVEN_OK)
        return from(opened.err);
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, sizeof(host_file), _Alignof(host_file));
    if (room.err != PROVEN_OK) {
        (void)proven_fs_close(opened.value);
        return PINAX_ERR_FULL;
    }
    host_file *file = (host_file *)(void *)room.value.ptr;
    file->in = (reader){opened.value, file->bytes, 0, 0, sizeof file->bytes, false};
    file->pipe = NULL;
    pinax_handle handle;
    const pinax_err err = pinax_handle_new(pinax_call_state(call), file_type(user), file, &handle);
    if (err != PINAX_OK) {
        file_close_object(file, NULL);
        return err;
    }
    args->ri[0] = (int64_t)handle;
    return PINAX_OK;
}
static pinax_err file_open_read(pinax_call *call, void *user, const pinax_args *args) {
    return file_open(call, user, args, PROVEN_FS_READ);
}
static pinax_err file_open_write(pinax_call *call, void *user, const pinax_args *args) {
    return file_open(call, user, args, (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
}
static pinax_err file_write(pinax_call *call, void *user, const pinax_args *args) {
    host_file *file = pinax_handle_get(pinax_call_state(call), file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE;
    return from(proven_fs_write_all(file->in.file, (proven_mem_view_t){args->s[0].ptr, args->s[0].len}));
}
static pinax_err file_read_line(pinax_call *call, void *user, const pinax_args *args) {
    host_file *file = pinax_handle_get(pinax_call_state(call), file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE;
    return line_from(call, &file->in, &args->rs[0]);
}
static pinax_err file_read_line_into(pinax_call *call, void *user, const pinax_args *args) {
    host_file *file = pinax_handle_get(pinax_call_state(call), file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE;
    return line_into(call, &file->in, args->i[1]);
}
// (f file, buf buffer gives n int or fail): appends what one read gives to the buffer; n is 0
// at the end of the file. Fails with full when the buffer has no room left.
static pinax_err file_read(pinax_call *call, void *user, const pinax_args *args) {
    host_file *file = pinax_handle_get(pinax_call_state(call), file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE;
    const pinax_mem room = pinax_call_buffer(call, args->i[1], 0);
    if (!room.len)
        return PINAX_ERR_FULL;
    reader *in = &file->in;
    if (in->at < in->end) { // what a line read took ahead comes first
        const size_t n = in->end - in->at < room.len ? in->end - in->at : room.len;
        memcpy(room.ptr, in->ahead + in->at, n);
        in->at += n;
        (void)pinax_call_buffer(call, args->i[1], n);
        args->ri[0] = (int64_t)n;
        return PINAX_OK;
    }
    const proven_result_size_t got = proven_fs_read(in->file, (proven_mem_mut_t){room.ptr, room.len});
    if (got.err != PROVEN_OK && got.err != PROVEN_ERR_EOF)
        return from(got.err);
    const size_t n = got.err == PROVEN_ERR_EOF ? 0 : got.value;
    (void)pinax_call_buffer(call, args->i[1], n);
    args->ri[0] = (int64_t)n;
    return PINAX_OK;
}
static pinax_err file_close(pinax_call *call, void *user, const pinax_args *args) {
    pinax_state *state = pinax_call_state(call);
    host_file *file = pinax_handle_get(state, file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE; // a fault: the word has no `or fail`
    const pinax_err err = pinax_handle_release(state, file_type(user), (pinax_handle)args->i[0]);
    file_close_object(file, NULL);
    return err;
}
// The whole file as one string in the caller's region.
static pinax_err file_read_all(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_file_t opened = proven_fs_open(heap, view_of(args->s[0]), PROVEN_FS_READ);
    if (opened.err != PROVEN_OK)
        return from(opened.err);
    pinax_err err = PINAX_OK;
    const proven_result_size_t size = proven_fs_size(opened.value);
    pinax_mem room = {NULL, 0};
    if (size.err != PROVEN_OK)
        err = from(size.err);
    else if (size.value && !(room = pinax_call_alloc(call, size.value)).ptr)
        err = PINAX_ERR_FULL;
    size_t have = 0;
    while (err == PINAX_OK && have < room.len) {
        const proven_result_size_t got =
            proven_fs_read(opened.value, (proven_mem_mut_t){(proven_byte_t *)room.ptr + have, room.len - have});
        if (got.err == PROVEN_ERR_EOF || (got.err == PROVEN_OK && got.value == 0))
            break;
        else if (got.err != PROVEN_OK)
            err = from(got.err);
        else
            have += got.value;
    }
    (void)proven_fs_close(opened.value);
    if (err != PINAX_OK)
        return err;
    args->rs[0].ptr = room.ptr;
    args->rs[0].len = have;
    return PINAX_OK;
}

// (path str, buf buffer or fail): the whole file appended to the buffer. A file larger than
// the room left fails with full before anything is read.
static pinax_err file_read_all_into(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_file_t opened = proven_fs_open(heap, view_of(args->s[0]), PROVEN_FS_READ);
    if (opened.err != PROVEN_OK)
        return from(opened.err);
    pinax_err err = PINAX_OK;
    const proven_result_size_t size = proven_fs_size(opened.value);
    const pinax_mem room = pinax_call_buffer(call, args->i[0], 0);
    if (size.err != PROVEN_OK)
        err = from(size.err);
    else if (size.value > room.len)
        err = PINAX_ERR_FULL;
    size_t have = 0;
    while (err == PINAX_OK && have < size.value) {
        const proven_result_size_t got = proven_fs_read(
            opened.value, (proven_mem_mut_t){(proven_byte_t *)room.ptr + have, size.value - have});
        if (got.err == PROVEN_ERR_EOF || (got.err == PROVEN_OK && got.value == 0))
            break;
        else if (got.err != PROVEN_OK)
            err = from(got.err);
        else
            have += got.value;
    }
    (void)proven_fs_close(opened.value);
    if (err == PINAX_OK)
        (void)pinax_call_buffer(call, args->i[0], have);
    return err;
}

// ---- clock --------------------------------------------------------------------------------

static pinax_err clock_now_ms(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = proven_time_now() / 1000000;
    return PINAX_OK;
}
static pinax_err clock_monotonic_ns(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = proven_time_monotonic_now();
    return PINAX_OK;
}
static pinax_err clock_sleep(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    pnx_streams_flush(); // what was written before the wait shows during it
    if (args->i[0] < 0 || args->i[0] > UINT32_MAX)
        return PINAX_ERR_RANGE;
    proven_time_sleep((proven_u32)args->i[0]);
    return PINAX_OK;
}

// ---- random -------------------------------------------------------------------------------

// One generator for the process. States on the threads of a loop (pinax_loop.h) call these words
// at once, so the generator is used under a lock: held for a few instructions, a spin will do.
static proven_xoshiro256ss_t generator;
static bool                  generator_seeded;
static atomic_flag           generator_lock = ATOMIC_FLAG_INIT;

static proven_rng_t rng_take(void) {
    while (atomic_flag_test_and_set_explicit(&generator_lock, memory_order_acquire)) {
    }
    if (!generator_seeded) {
        proven_xoshiro256ss_seed(&generator, proven_random_u64());
        generator_seeded = true;
    }
    return proven_xoshiro256ss_rng(&generator);
}
static void rng_give(void) {
    atomic_flag_clear_explicit(&generator_lock, memory_order_release);
}
static pinax_err random_next(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = (int64_t)proven_rng_u64(rng_take());
    rng_give();
    return PINAX_OK;
}
static pinax_err random_below(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    if (args->i[0] <= 0)
        return PINAX_ERR_RANGE;
    args->ri[0] = (int64_t)proven_rng_below(rng_take(), (proven_u64)args->i[0]);
    rng_give();
    return PINAX_OK;
}
static pinax_err random_seed(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    (void)rng_take();
    proven_xoshiro256ss_seed(&generator, (proven_u64)args->i[0]);
    rng_give();
    return PINAX_OK;
}

// ---- env ----------------------------------------------------------------------------------

static int                host_arg_count;
static const char *const *host_args;

void pinax_host_set_args(int count, const char *const *args) {
    host_arg_count = count;
    host_args = args;
}

static pinax_err env_arg_count(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = host_arg_count;
    return PINAX_OK;
}
static pinax_err env_arg(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    if (args->i[0] < 0 || args->i[0] >= host_arg_count)
        return PINAX_ERR_RANGE;
    const char *text = host_args[args->i[0]];
    size_t n = 0;
    while (text[n])
        ++n;
    args->rs[0].ptr = (const unsigned char *)text; // KEEP_STR: the arguments outlive the state
    args->rs[0].len = n;
    return PINAX_OK;
}
static pinax_err env_getenv(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    proven_allocator_t heap = proven_heap_allocator();
    proven_result_u8str_t got = proven_env_get(heap, view_of(args->s[0]));
    if (got.err != PROVEN_OK)
        return from(got.err);
    const proven_u8str_view_t value = proven_u8str_as_view(&got.value);
    pinax_mem room = {NULL, 0};
    pinax_err err = PINAX_OK;
    if (value.size && !(room = pinax_call_alloc(call, value.size)).ptr)
        err = PINAX_ERR_FULL;
    for (size_t k = 0; err == PINAX_OK && k < value.size; ++k)
        ((unsigned char *)room.ptr)[k] = value.ptr[k];
    proven_u8str_destroy(heap, &got.value);
    if (err != PINAX_OK)
        return err;
    args->rs[0].ptr = room.ptr;
    args->rs[0].len = value.size;
    return PINAX_OK;
}

// ---- sh (RFC-0040 part B) -------------------------------------------------------------------

#if defined(_WIN32) || defined(_WIN64)
#define sh_popen  _popen
#define sh_pclose _pclose
#else
#define sh_popen  popen
#define sh_pclose pclose
#endif

// The status of the last command that ended, and whether sh.exit was asked for: of the thread,
// as the commands it ran.
static _Thread_local int  sh_status = 0;
static _Thread_local bool sh_exit_asked = false;
static _Thread_local int  sh_exit_with = 0;

// What the system gave for an ended command, as a status: its exit code, 128 and the signal when a
// signal ended it.
static void sh_ended(int raw) {
#if defined(_WIN32) || defined(_WIN64)
    sh_status = raw;
#else
    sh_status = raw == -1 ? -1 : WIFEXITED(raw) ? WEXITSTATUS(raw) : WIFSIGNALED(raw) ? 128 + WTERMSIG(raw) : raw;
#endif
}

// The command as C text, on the heap; NULL when it holds a zero byte or there is no room.
static char *sh_text(pinax_str s) {
    if (memchr(s.ptr, 0, s.len))
        return NULL;
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, s.len + 1, 1);
    if (room.err != PROVEN_OK)
        return NULL;
    char *text = (char *)(void *)room.value.ptr;
    memcpy(text, s.ptr, s.len);
    text[s.len] = '\0';
    return text;
}
static void sh_free(char *text) {
    proven_allocator_t heap = proven_heap_allocator();
    heap.free_fn(heap.ctx, text);
}

// (cmd str gives status int or fail): through the system shell; what the program wrote comes first
static pinax_err sh_run(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    char *cmd = sh_text(args->s[0]);
    if (!cmd)
        return PINAX_ERR_ARGUMENT;
    pnx_streams_flush();
    fflush(stdout);
    const int raw = system(cmd);
    sh_free(cmd);
    if (raw == -1)
        return PINAX_ERR_FAILED; // the shell could not be started
    sh_ended(raw);
    args->ri[0] = sh_status;
    return PINAX_OK;
}

// The output of a command, read to its end: appended to the buffer `buffer`, or (buffer < 0)
// collected on the heap and copied into the caller's region.
static pinax_err sh_collect(pinax_call *call, pinax_str command, int64_t buffer, pinax_str *out) {
    char *cmd = sh_text(command);
    if (!cmd)
        return PINAX_ERR_ARGUMENT;
    pnx_streams_flush();
    fflush(stdout);
    FILE *pipe = sh_popen(cmd, "r");
    sh_free(cmd);
    if (!pipe)
        return PINAX_ERR_FAILED;
    proven_allocator_t heap = proven_heap_allocator();
    unsigned char     *all = NULL, chunk[4096];
    size_t             n = 0, cap = 0;
    pinax_err          err = PINAX_OK;
    for (;;) {
        const size_t got = fread(chunk, 1, sizeof chunk, pipe);
        if (got == 0)
            break;
        if (err != PINAX_OK)
            continue; // read to the end, so that the command is not stopped by a closed pipe
        if (buffer >= 0) {
            const pinax_mem room = pinax_call_buffer(call, buffer, 0);
            const size_t    fits = got < room.len ? got : room.len;
            memcpy(room.ptr, chunk, fits);
            (void)pinax_call_buffer(call, buffer, fits);
            if (fits < got)
                err = PINAX_ERR_FULL;
            continue;
        }
        if (n + got > cap) {
            const size_t bigger = cap ? cap * 2 : 8192;
            const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, bigger < n + got ? n + got : bigger, 1);
            if (room.err != PROVEN_OK) {
                err = PINAX_ERR_FULL;
                continue;
            }
            if (n)
                memcpy(room.value.ptr, all, n);
            if (all)
                heap.free_fn(heap.ctx, all);
            all = room.value.ptr;
            cap = room.value.size;
        }
        memcpy(all + n, chunk, got);
        n += got;
    }
    sh_ended(sh_pclose(pipe));
    if (buffer < 0 && err == PINAX_OK) {
        const pinax_mem room = n ? pinax_call_alloc(call, n) : (pinax_mem){NULL, 0};
        if (n && !room.ptr)
            err = PINAX_ERR_FULL;
        else if (n)
            memcpy(room.ptr, all, n);
        if (err == PINAX_OK)
            *out = (pinax_str){room.ptr, n};
    }
    if (all)
        heap.free_fn(heap.ctx, all);
    return err;
}
// (cmd str gives out str or fail): what the command wrote on its standard output, as written
static pinax_err sh_output(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    return sh_collect(call, args->s[0], -1, &args->rs[0]);
}
// (cmd str, buf buffer gives status int or fail): the same, appended to a buffer; full when it does not fit
static pinax_err sh_output_into(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    pinax_str none;
    const pinax_err err = sh_collect(call, args->s[0], args->i[0], &none);
    if (err == PINAX_OK)
        args->ri[0] = sh_status;
    return err;
}

// (cmd str gives owned file or fail): a pipe from (read) or to (write) the command, as a file
static pinax_err sh_open(pinax_call *call, void *user, const pinax_args *args, bool write) {
    char *cmd = sh_text(args->s[0]);
    if (!cmd)
        return PINAX_ERR_ARGUMENT;
    pnx_streams_flush();
    fflush(stdout);
#if defined(_WIN32) || defined(_WIN64)
    FILE *pipe = sh_popen(cmd, write ? "wb" : "rb");
#else
    FILE *pipe = sh_popen(cmd, write ? "w" : "r");
#endif
    sh_free(cmd);
    if (!pipe)
        return PINAX_ERR_FAILED;
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, sizeof(host_file), _Alignof(host_file));
    if (room.err != PROVEN_OK) {
        sh_ended(sh_pclose(pipe));
        return PINAX_ERR_FULL;
    }
    host_file    *file = (host_file *)(void *)room.value.ptr;
    proven_file_t through = {0};
#if defined(_WIN32) || defined(_WIN64)
    through.internal.ptr = (void *)_get_osfhandle(_fileno(pipe));
#else
    through.internal.fd = fileno(pipe);
#endif
    file->in = (reader){through, file->bytes, 0, 0, sizeof file->bytes, false};
    file->pipe = pipe;
    pinax_handle handle;
    const pinax_err err = pinax_handle_new(pinax_call_state(call), file_type(user), file, &handle);
    if (err != PINAX_OK) {
        file_close_object(file, NULL);
        return err;
    }
    args->ri[0] = (int64_t)handle;
    return PINAX_OK;
}
static pinax_err sh_open_read(pinax_call *call, void *user, const pinax_args *args) {
    return sh_open(call, user, args, false);
}
static pinax_err sh_open_write(pinax_call *call, void *user, const pinax_args *args) {
    return sh_open(call, user, args, true);
}

// (gives int): the status of the last command that ended (sh.run, sh.output, a pipe closed)
static pinax_err sh_status_word(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = sh_status;
    return PINAX_OK;
}

// (s str gives str): `s` as one word for the system shell. /bin/sh: in single quotes, a quote as
// '\''. cmd.exe: in double quotes, a double quote doubled; % and ! keep their meaning there.
static pinax_err sh_quote(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    const pinax_str s = args->s[0];
#if defined(_WIN32) || defined(_WIN64)
    const unsigned char mark = '"';
    size_t extra = 2;
    for (size_t k = 0; k < s.len; ++k)
        extra += s.ptr[k] == '"';
#else
    const unsigned char mark = '\'';
    size_t extra = 2;
    for (size_t k = 0; k < s.len; ++k)
        extra += s.ptr[k] == '\'' ? 3 : 0;
#endif
    const pinax_mem room = pinax_call_alloc(call, s.len + extra);
    if (!room.ptr)
        return PINAX_ERR_FULL; // a fault: the word has no `or fail`
    unsigned char *to = room.ptr;
    size_t         n = 0;
    to[n++] = mark;
    for (size_t k = 0; k < s.len; ++k) {
#if defined(_WIN32) || defined(_WIN64)
        if (s.ptr[k] == '"')
            to[n++] = '"';
        to[n++] = s.ptr[k];
#else
        if (s.ptr[k] == '\'') {
            memcpy(to + n, "'\\''", 4);
            n += 4;
        } else {
            to[n++] = s.ptr[k];
        }
#endif
    }
    to[n++] = mark;
    args->rs[0] = (pinax_str){to, n};
    return PINAX_OK;
}

// (path str or fail), (gives path str or fail): the working directory of the process
static pinax_err sh_cd(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    char *path = sh_text(args->s[0]);
    if (!path)
        return PINAX_ERR_ARGUMENT;
#if defined(_WIN32) || defined(_WIN64)
    const int done = _chdir(path);
#else
    const int done = chdir(path);
#endif
    sh_free(path);
    return done == 0 ? PINAX_OK : PINAX_ERR_NOT_FOUND;
}
static pinax_err sh_cwd(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    char here[4096];
#if defined(_WIN32) || defined(_WIN64)
    if (!_getcwd(here, (int)sizeof here))
#else
    if (!getcwd(here, sizeof here))
#endif
        return PINAX_ERR_FAILED;
    const size_t    n = strlen(here);
    const pinax_mem room = pinax_call_alloc(call, n ? n : 1);
    if (!room.ptr)
        return PINAX_ERR_FULL;
    memcpy(room.ptr, here, n);
    args->rs[0] = (pinax_str){room.ptr, n};
    return PINAX_OK;
}

// (name str, value str or fail): for the commands that follow (and env.getenv)
static pinax_err sh_setenv(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    char *name = sh_text(args->s[0]), *value = sh_text(args->s[1]);
    pinax_err err = name && value && args->s[0].len && !memchr(args->s[0].ptr, '=', args->s[0].len) ? PINAX_OK
                                                                                                      : PINAX_ERR_ARGUMENT;
#if defined(_WIN32) || defined(_WIN64)
    if (err == PINAX_OK && _putenv_s(name, value) != 0)
#else
    if (err == PINAX_OK && setenv(name, value, 1) != 0)
#endif
        err = PINAX_ERR_FAILED;
    if (name)
        sh_free(name);
    if (value)
        sh_free(value);
    return err;
}

// (status int): ends the program with that status, 0 to 255. The run stops here
// (PINAX_ERR_INTERRUPTED), and pinax_host_exit_asked tells the host the status.
static pinax_err sh_exit(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    sh_exit_asked = true;
    sh_exit_with = args->i[0] < 0 ? 0 : args->i[0] > 255 ? 255 : (int)args->i[0];
    return PINAX_ERR_INTERRUPTED;
}

bool pinax_host_exit_asked(int *status) {
    if (!sh_exit_asked)
        return false;
    if (status)
        *status = sh_exit_with;
    sh_exit_asked = false; // told once: a later interruption is the budget's or the host's
    return true;
}

// ---- registration -------------------------------------------------------------------------

static bool same(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

pinax_err pinax_host_register(pinax_state *state, const char *module) {
    if (!state || !module)
        return PINAX_ERR_ARGUMENT;
    if (same(module, "io")) {
        const pinax_word_def words[] = {
            {"print", "(s str)", io_print, NULL, PINAX_WORD_WRITES},
            {"println", "(s str)", io_println, NULL, PINAX_WORD_WRITES},
            {"eprint", "(s str)", io_eprint, NULL, PINAX_WORD_WRITES},
            {"read_line", "(gives line str or fail)", io_read_line, NULL, PINAX_WORD_READS},
            {"read_line_into", "(buf buffer or fail)", io_read_line_into, NULL, PINAX_WORD_READS},
        };
        return pinax_register(state, "io", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "file")) {
        uint32_t type;
        const pinax_err err = pinax_handle_type(state, "file", file_close_object, NULL, &type);
        if (err != PINAX_OK)
            return err;
        void *tag = (void *)(uintptr_t)type;
        const pinax_word_def words[] = {
            {"open_read", "(path str gives owned file or fail)", file_open_read, tag, PINAX_WORD_READS},
            {"open_write", "(path str gives owned file or fail)", file_open_write, tag, PINAX_WORD_WRITES},
            {"write", "(s str, f file or fail)", file_write, tag, PINAX_WORD_WRITES},
            {"read_line", "(f file gives line str or fail)", file_read_line, tag, PINAX_WORD_READS},
            {"read", "(f file, buf buffer gives n int or fail)", file_read, tag, PINAX_WORD_READS},
            {"read_all", "(path str gives body str or fail)", file_read_all, tag, PINAX_WORD_READS},
            {"read_line_into", "(f file, buf buffer or fail)", file_read_line_into, tag, PINAX_WORD_READS},
            {"read_all_into", "(path str, buf buffer or fail)", file_read_all_into, tag, PINAX_WORD_READS},
            {"close", "(f owned file)", file_close, tag, 0},
        };
        return pinax_register(state, "file", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "clock")) {
        const pinax_word_def words[] = {
            {"now_ms", "(gives int)", clock_now_ms, NULL, PINAX_WORD_READS},
            {"monotonic_ns", "(gives int)", clock_monotonic_ns, NULL, PINAX_WORD_READS},
            {"sleep", "(ms int)", clock_sleep, NULL, PINAX_WORD_READS},
        };
        return pinax_register(state, "clock", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "random")) {
        const pinax_word_def words[] = {
            {"random", "(gives int)", random_next, NULL, 0},
            {"random_below", "(n int gives int)", random_below, NULL, 0},
            {"seed", "(n int)", random_seed, NULL, 0},
        };
        return pinax_register(state, "random", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "sh")) { // shares the handle type of file: a pipe is a file
        uint32_t type;
        const pinax_err err = pinax_handle_type(state, "file", file_close_object, NULL, &type);
        if (err != PINAX_OK)
            return err;
        void *tag = (void *)(uintptr_t)type;
        const pinax_word_def words[] = {
            {"run", "(cmd str gives status int or fail)", sh_run, NULL, 0},
            {"output", "(cmd str gives out str or fail)", sh_output, NULL, 0},
            {"output_into", "(cmd str, buf buffer gives status int or fail)", sh_output_into, NULL, 0},
            {"open_read", "(cmd str gives owned file or fail)", sh_open_read, tag, 0},
            {"open_write", "(cmd str gives owned file or fail)", sh_open_write, tag, 0},
            {"status", "(gives int)", sh_status_word, NULL, PINAX_WORD_READS},
            {"quote", "(s str gives str)", sh_quote, NULL, PINAX_WORD_PURE},
            {"cd", "(path str or fail)", sh_cd, NULL, 0},
            {"cwd", "(gives path str or fail)", sh_cwd, NULL, PINAX_WORD_READS},
            {"setenv", "(name str, value str or fail)", sh_setenv, NULL, 0},
            {"exit", "(status int)", sh_exit, NULL, 0},
        };
        return pinax_register(state, "sh", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "env")) {
        const pinax_word_def words[] = {
            {"arg_count", "(gives int)", env_arg_count, NULL, PINAX_WORD_READS},
            {"arg", "(i int gives s str or fail)", env_arg, NULL, PINAX_WORD_KEEP_STR | PINAX_WORD_READS},
            {"getenv", "(name str gives s str or fail)", env_getenv, NULL, PINAX_WORD_READS},
        };
        return pinax_register(state, "env", words, sizeof words / sizeof words[0]);
    }
    return PINAX_ERR_NOT_FOUND;
}

void pinax_host_barrier(void *user) {
    (void)user;
    pnx_streams_flush(); // the files are written as they go: only standard output is held
}

pinax_err pinax_host_register_all(pinax_state *state) {
    static const char *const names[] = {"io", "file", "clock", "random", "env"};
    for (unsigned k = 0; k < sizeof names / sizeof names[0]; ++k) {
        const pinax_err err = pinax_host_register(state, names[k]);
        if (err != PINAX_OK)
            return err;
    }
    pinax_on_barrier(state, pinax_host_barrier, NULL);
    return PINAX_OK;
}

// ---- a file mapped read-only, for pinax_share_file() (RFC-0020 section 4.2) ----

struct pinax_mapping {
    proven_mmap_t map;
};

pinax_err pinax_map_file(const char *path, pinax_str *bytes, pinax_mapping **out) {
    if (!path || !bytes || !out)
        return PINAX_ERR_ARGUMENT;
    *out = NULL;
    *bytes = (pinax_str){0};
    proven_allocator_t         heap = proven_heap_allocator();
    size_t                     len = 0;
    while (path[len])
        ++len;
    const proven_result_file_t opened = proven_fs_open(heap, (proven_u8str_view_t){(const unsigned char *)path, len},
                                                       PROVEN_FS_READ);
    if (opened.err != PROVEN_OK)
        return from(opened.err);
    const proven_result_size_t length = proven_fs_size(opened.value);
    if (length.err != PROVEN_OK || length.value == 0) { // nothing to map: no share file either
        (void)proven_fs_close(opened.value);
        return length.err != PROVEN_OK ? from(length.err) : PINAX_ERR_FORMAT;
    }
    // shared: every process that maps the file reads the same pages
    const proven_result_mmap_t mapped = proven_mmap_create(opened.value, 0, 0, PROVEN_MMAP_READ, PROVEN_MMAP_SHARED);
    if (mapped.err != PROVEN_OK) {
        (void)proven_fs_close(opened.value);
        return from(mapped.err);
    }
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, sizeof(pinax_mapping), _Alignof(pinax_mapping));
    if (room.err != PROVEN_OK) {
        proven_mmap_t m = mapped.value;
        (void)proven_mmap_destroy(&m);
        (void)proven_fs_close(opened.value);
        return PINAX_ERR_FULL;
    }
    pinax_mapping *m = (pinax_mapping *)(void *)room.value.ptr;
    m->map = mapped.value;
    *bytes = (pinax_str){(const unsigned char *)m->map.ptr, (size_t)m->map.size};
    *out = m;
    return PINAX_OK;
}

void pinax_unmap_file(pinax_mapping *mapping) {
    if (!mapping)
        return;
    const proven_file_t file = mapping->map.file;
    (void)proven_mmap_destroy(&mapping->map);
    (void)proven_fs_close(file);
    proven_allocator_t heap = proven_heap_allocator();
    heap.free_fn(heap.ctx, mapping);
}

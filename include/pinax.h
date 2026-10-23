/*
 * pinax.h - host interface of the Pinax embeddable scripting language.
 *
 * The whole interface is this one header. It needs only <stddef.h> and <stdint.h>.
 *
 * Threads: a state has no global or thread-local data. Any thread may use a state,
 * but only one at a time. Two states share nothing.
 *
 * Memory: a state lives entirely inside the block given to pinax_open(). Pinax never
 * asks the system for memory. Closing a state does not free the block; the host owns it.
 */
#ifndef PINAX_H
#define PINAX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINAX_VERSION_MAJOR 0
#define PINAX_VERSION_MINOR 9
#define PINAX_VERSION_PATCH 1
#define PINAX_FORMAT 2 /* source, bytecode and image format; bumped by breaking changes */

#ifndef PINAX_API
#define PINAX_API
#endif

/* ---- basic types ------------------------------------------------------------------ */

typedef struct pinax_state pinax_state; /* opaque; lives at the start of the block */
typedef struct pinax_call pinax_call;   /* one running host-word call */

typedef struct { void *ptr; size_t len; } pinax_mem;                /* writable bytes */
typedef struct { const unsigned char *ptr; size_t len; } pinax_str; /* byte view, not NUL-terminated */
/* Names (words, globals, modules, handle types) are NUL-terminated C strings; content
 * (source, bytecode, images, messages, string values) is a pinax_str. */

typedef uint64_t pinax_handle; /* index and generation of a host object; 0 is never valid */
typedef uint32_t pinax_word;   /* a word found by pinax_find(); stable until the state closes */
typedef uint64_t pinax_token;  /* one pending host operation */

typedef enum pinax_err {
    PINAX_OK = 0,
    /* outcomes that are not failures */
    PINAX_SUSPENDED,    /* instruction budget ran out; pinax_resume() continues */
    PINAX_WAITING,      /* every runnable task waits for a host operation */
    PINAX_PENDING,      /* returned by an awaitable host word: result comes later */
    /* a fault that ended the script, or a failure it passed up to the host */
    PINAX_ERR_FAILED = 0x100,
    PINAX_ERR_OVERFLOW, /* integer overflow */
    PINAX_ERR_DIV_ZERO, /* division by zero */
    PINAX_ERR_RANGE,    /* index outside a declared capacity */
    PINAX_ERR_FULL,     /* a declared capacity is used up */
    PINAX_ERR_DEPTH,    /* call depth limit */
    PINAX_ERR_STALE,    /* handle or pool slot used after release */
    PINAX_ERR_CANCELLED,
    PINAX_ERR_INTERRUPTED,
    PINAX_ERR_DEADLOCK, /* every task left waits for a queue that nothing will change */
    /* failures of the interface itself */
    PINAX_ERR_ARGUMENT = 0x200,
    PINAX_ERR_NO_ROOM,   /* the block is too small; see pinax_needed() */
    PINAX_ERR_SYNTAX,    /* load: source does not parse */
    PINAX_ERR_TYPE,      /* load: stack effect or type check failed */
    PINAX_ERR_NOT_FOUND,
    PINAX_ERR_SIGNATURE, /* a word exists but its signature differs from the one asked for */
    PINAX_ERR_FORMAT,    /* bytecode or image of another format, layout or code */
    PINAX_ERR_STATE      /* call not allowed now (for example load while running) */
} pinax_err;

/* ---- opening and closing ---------------------------------------------------------- */

/* Capacities. Zero means "default". struct_size lets the struct grow compatibly. */
typedef struct pinax_config {
    size_t   struct_size;   /* sizeof(pinax_config) */
    uint32_t int_slots;     /* I bank: 64-bit slots */
    uint32_t str_slots;     /* S bank: string view slots */
    uint32_t float_slots;   /* F bank; 0 and no float in scripts = no bank */
    uint32_t region_bytes;  /* byte region of the main task */
    uint32_t handle_slots;  /* host objects alive at once */
    uint64_t budget;        /* instructions per call or resume; 0 = unlimited */
    /* load diagnostics; may be NULL */
    void (*diag)(void *user, pinax_str file, uint32_t line, uint32_t column, pinax_str message);
    void *diag_user;
    /* Key of the hash that lookup indexes use. Give 16 random bytes when scripts index text
     * that comes from outside (names, words, request fields): without a secret key, whoever
     * chooses the keys can make every lookup slow. All zero means a fixed built-in key: fine
     * for trusted data, no protection otherwise. The key is kept in the state and its images. */
    unsigned char hash_key[16];
} pinax_config;

PINAX_API pinax_err pinax_open(pinax_mem block, const pinax_config *config, pinax_state **out);
PINAX_API void      pinax_close(pinax_state *state); /* closes the handles still open */

/* After PINAX_ERR_NO_ROOM: at least the block size that would have been enough (the size of
 * what was being taken when the block ran out; a load may take more after that). */
PINAX_API size_t    pinax_needed(const pinax_state *state);
/* Text of the last failure, valid until the next call on this state. */
PINAX_API pinax_str pinax_error_text(const pinax_state *state);

/* ---- host words ------------------------------------------------------------------- */

/*
 * Arguments and results of one call, by type. A word declared
 *     (name str, count int, flags int gives total int)
 * finds name in s[0], count in i[0], flags in i[1], and stores total in ri[0].
 * The integer pointers go straight into the state's bank: nothing is converted or copied.
 * The string arrays exist only for this call (a script holds a string as a segment and an
 * offset, never as an address); the bytes they point to are not copied.
 * bool, characters, handles and distinct types over int all count as int.
 */
typedef struct pinax_args {
    const int64_t   *i;  int64_t   *ri;
    const pinax_str *s;  pinax_str *rs;
    const double    *f;  double    *rf;
} pinax_args;

typedef pinax_err (*pinax_host_fn)(pinax_call *call, void *user, const pinax_args *args);

enum {
    PINAX_WORD_PURE      = 1u << 0, /* no effect outside its results; may be folded or run in parallel */
    PINAX_WORD_AWAITABLE = 1u << 1, /* may return PINAX_PENDING */
    PINAX_WORD_KEEP_STR  = 1u << 2  /* string results point to memory that outlives the state */
};

typedef struct pinax_word_def {
    const char   *name;      /* word name as scripts see it */
    const char   *signature; /* "(a int, b str gives r int)"; checked once, here */
    pinax_host_fn fn;
    void         *user;
    uint32_t      flags;
} pinax_word_def;

/* Makes the words visible to scripts as module `module`. Before the scripts that use them load.
 * On a state opened from an image (or cloned), it gives the image's host words again: each
 * must be one of them, with the same signature and flags (else PINAX_ERR_NOT_FOUND or
 * PINAX_ERR_SIGNATURE). Until all of them are given, runs answer PINAX_ERR_NOT_FOUND. */
PINAX_API pinax_err pinax_register(pinax_state *state, const char *module,
                                   const pinax_word_def *defs, size_t count);

/* Inside a host word. A string result not flagged KEEP_STR is copied into the caller's region. */
PINAX_API pinax_state *pinax_call_state(pinax_call *call);
PINAX_API pinax_mem    pinax_call_alloc(pinax_call *call, size_t bytes); /* caller's region; len 0 = full */
PINAX_API pinax_token  pinax_call_pending(pinax_call *call);             /* then return PINAX_PENDING */
/* A parameter written `buffer` in a host word's signature takes a buffer the script declared;
 * the word finds it in args->i like an integer and hands that value to this function. It
 * counts `written` more bytes as part of the buffer's contents and gives the free room after
 * them: ask with 0, fill the room, then tell how much was filled. When `written` is more than
 * the room, or the value is not a buffer, nothing changes and the answer is {NULL, 0}. To read
 * a buffer, let the script pass its contents as a str. */
PINAX_API pinax_mem    pinax_call_buffer(pinax_call *call, int64_t buffer, size_t written);

/* A host word registered PINAX_WORD_AWAITABLE may start an operation, take a token for it with
 * pinax_call_pending() and return PINAX_PENDING. The run that called it then waits: a task
 * gives way to the others, and when nothing else can run pinax_run() or pinax_resume() answers
 * PINAX_WAITING. The host finishes the operation with pinax_complete() and calls pinax_resume().
 *
 * Finishes a pending operation; the waiting task becomes runnable. results may be NULL on failure.
 * results->ri and results->rs hold the word's results from index 0; strings are copied. An
 * outcome other than PINAX_OK is the word's failure when its signature has `or fail`, and a
 * fault of the run otherwise. PINAX_ERR_NOT_FOUND: no run waits for this token (the run that
 * did has ended in a fault, say).
 * Like every call, only from the thread that is using the state: a completion that arrives on
 * another thread must be handed over by the host first. */
PINAX_API pinax_err pinax_complete(pinax_state *state, pinax_token token,
                                   pinax_err outcome, const pinax_args *results);

/* ---- C libraries named by scripts --------------------------------------------------- */

/*
 * A script's `extern name (...) from "lib"` asks the host for the function ("lib", "name").
 * With `as other` at the end the script calls it `other`; the symbol asked for is still
 * "name". Pinax does not load libraries itself: with no resolver, or when it returns NULL,
 * the load fails.
 * Only integer, handle and string arguments and one integer or handle result are callable
 * this way; anything else needs a host word.
 *
 * How the function is called: every integer and every handle's object pointer is one
 * intptr_t argument, in the order written; a `str` is two arguments, (const char *, size_t),
 * and is not NUL-terminated; a `cstr` is one const char * to a NUL-terminated copy that lives
 * for the call. At most eight C arguments. The result is read as one intptr_t, so a function
 * returning a narrower type (int), taking a double or a struct by value, or taking variable
 * arguments cannot be named: wrap it in a host word. A null pointer where a handle is the
 * result is a failure. A function given an `owned` handle takes the object over; the type's
 * close function is not called for it. A wrong signature in the script is undefined behaviour
 * in C: a host that runs scripts it does not trust sets no resolver.
 */
typedef void (*pinax_extern_fn)(void);
typedef pinax_extern_fn (*pinax_resolve_fn)(void *user, pinax_str library, pinax_str symbol);
/* On a state opened from an image, setting the resolver asks it at once for every extern
 * function of the image. */
PINAX_API void pinax_set_resolver(pinax_state *state, pinax_resolve_fn resolve, void *user);

/* ---- host objects ----------------------------------------------------------------- */

/* A kind of host pointer scripts can hold as a use-once value of type `name`. close may be NULL.
 * On a state opened from an image, a type of the image is given again by its name and gets its
 * number back; another name is refused. */
PINAX_API pinax_err pinax_handle_type(pinax_state *state, const char *name,
                                      void (*close)(void *object, void *user), void *user,
                                      uint32_t *out_type);
PINAX_API pinax_err pinax_handle_new(pinax_state *state, uint32_t type, void *object, pinax_handle *out);
PINAX_API void     *pinax_handle_get(pinax_state *state, uint32_t type, pinax_handle handle); /* NULL if stale */
PINAX_API pinax_err pinax_handle_release(pinax_state *state, uint32_t type, pinax_handle handle);

/* ---- host memory as globals --------------------------------------------------------- */

/* Gives the storage of a global the script declares `bound`: a scalar, an array (of integers,
 * of str(N), of records) or a buffer. `bytes` must be the declared size. A buffer's memory
 * begins with its length, a uint64_t, and its declared bytes follow (so `bound buffer b 100`
 * takes 108 bytes); the length found there at binding must not exceed the declared size.
 * Until a global is bound it has storage of its own. Bind again after pinax_image_open(). The
 * memory must outlive the state. Refused (PINAX_ERR_STATE) while a run is suspended or waits, or
 * from inside a host word: the run may hold views into the storage it has. So are pinax_share()
 * and pinax_share_file(). */
PINAX_API pinax_err pinax_bind(pinax_state *state, const char *global, void *memory, size_t bytes);

/* Where a declared global lives, to read or write it in place between calls. `number` is what a
 * word with a collection parameter (`l list int`, `q queue int`, `b buffer`) takes as that
 * argument, an integer. A number of a collection of another kind or element size is the fault
 * PINAX_ERR_STATE where the word uses it. */
typedef struct pinax_global {
    void    *ptr;
    size_t   element_bytes;
    size_t   capacity; /* declared element count; 1 for a scalar */
    pinax_str type;    /* type as written in the declaration */
    int64_t  number;
} pinax_global;
PINAX_API pinax_err pinax_global_find(pinax_state *state, const char *name, pinax_global *out);

/* Read-only sharing between states. A global the script declares `shared` (a scalar, array, list,
 * buffer or index) has no storage of its own: this gives it the storage of the global `from` of
 * `source`, without copying. Kind, type and capacity must be those of that declaration (else
 * PINAX_ERR_SIGNATURE); an index also needs the same hash_key, and its table shared first from
 * the same source. A list keeps the length it has now. From then on `source` is frozen: it does
 * not run or load any more (PINAX_ERR_STATE), so its bytes do not change, and states on other
 * threads may read them at the same time. It must stay open while a state uses what it lent.
 * Nothing a script does writes a shared global (refused at load). Until every shared global is
 * given, runs are refused with PINAX_ERR_NOT_FOUND; the code of a load reads them as empty. After
 * pinax_image_open() or pinax_clone(), give them again. */
PINAX_API pinax_err pinax_share(pinax_state *state, const char *global, pinax_state *source, const char *from);

/* The same from a file, for states in several processes. pinax_share_save() writes every global
 * of `source` that can be shared (not those it declares `shared` itself) into one file without
 * addresses; pinax_share_size() is its length. A process maps the file read-only (the host
 * library's pinax_map_file()) and gives a shared global from it with pinax_share_file(), with
 * the checks of pinax_share() (the hash_key is the file's). The file's head, the size of each
 * global, the lengths and every slot of an index are checked (PINAX_ERR_FORMAT); its bytes are
 * otherwise trusted as an image is. `file` starts 16-byte aligned and outlives the states that
 * read it; nothing is frozen. */
PINAX_API size_t    pinax_share_size(const pinax_state *source);
PINAX_API pinax_err pinax_share_save(const pinax_state *source, pinax_mem out, size_t *written);
PINAX_API pinax_err pinax_share_file(pinax_state *state, const char *global, pinax_str file, const char *from);

/* ---- loading ----------------------------------------------------------------------- */

enum {
    PINAX_LOAD_CODE = 0, /* declarations and words */
    PINAX_LOAD_DATA = 1  /* declarations and data only: no word definitions, nothing runs;
                            uses a small budget proportional to the source size, not config.budget */
};
/* Compiles the source and then runs, once, the code written outside definitions.
 *  - A state takes any number of sources, one after the other; each adds to the program. `name`
 *    is the name of the module the source is: a later source writes `use name` and then
 *    name.word for what this one marked `export` (a data source exports all it declares). A
 *    module must be loaded before a source that uses it. A name that is not written like a word
 *    (a path, say) or NULL loads a source no other can use; a name already taken by a module
 *    is refused with PINAX_ERR_STATE.
 *  - A fault in that code fails the load with the fault's number and nothing of this load stays
 *    loaded: no word, no global. The same holds for every other load error. The sources loaded
 *    before stay as they were, except for what the failed code stored into their globals; what
 *    host words did before the fault is not undone either. The state can be loaded again.
 *  - PINAX_LOAD_CODE: that code runs on config.budget like any other. When the budget runs out
 *    pinax_load() returns PINAX_SUSPENDED and pinax_resume() continues it; until it has finished
 *    pinax_run() is refused, and a fault met while resuming fails the load as above.
 *  - PINAX_LOAD_DATA: a source that does not finish inside its own small budget fails the load. */
PINAX_API pinax_err pinax_load(pinax_state *state, const char *name, pinax_str source, uint32_t mode);
PINAX_API pinax_err pinax_load_bytecode(pinax_state *state, pinax_str bytecode);
/* One line at the top level, as in an interactive session (the text may be several lines).
 *  - All the texts given to pinax_eval() are one module, the session: a line sees what the
 *    lines before it defined and the modules they opened with `use`. Sources loaded with
 *    pinax_load() do not see the session's names.
 *  - What the line leaves on the stack is read with pinax_eval_count() and pinax_eval_value().
 *  - The tool words that exist only here answer through config.diag: `list_words` and `stack`
 *    (the types on the stack at that place: "stack: int str") with line 0, one message each;
 *    `see`, after which every definition in the text is listed, one message for each of its
 *    lines (that line number, column 0): "stack: int str". `redefine word NAME ...` replaces a
 *    word an earlier text defined, keeping its signature; code that calls it runs the new one.
 *  - A text that ends inside a definition or a block answers PINAX_ERR_SYNTAX with an error
 *    text that begins "unfinished:"; give the longer text when the rest has come.
 *  - A text that is refused or ends in a fault leaves nothing behind. A name that exists is
 *    not defined again. */
PINAX_API pinax_err pinax_eval(pinax_state *state, pinax_str line);

/* What the last pinax_eval() that answered PINAX_OK left on the stack: pinax_eval_count()
 * values, the deepest at index 0. Strings point into the state's memory and stay valid until
 * the next load, eval or run; so does the type's name. */
enum {
    PINAX_VALUE_INT = 1, /* also characters, handles and distinct types over int */
    PINAX_VALUE_BOOL,
    PINAX_VALUE_STR,     /* also distinct types over str */
    PINAX_VALUE_ERR
};
typedef struct pinax_value {
    uint32_t  kind;  /* PINAX_VALUE_... */
    int64_t   i;     /* the integer; 0 or 1 for a bool; the error's number for an err */
    pinax_str s;     /* the bytes of a str; the error's name ("not_found") for an err */
    pinax_str type;  /* the type as the source writes it: "int", "str", "file", "metres" */
} pinax_value;
PINAX_API uint32_t  pinax_eval_count(const pinax_state *state);
PINAX_API pinax_err pinax_eval_value(const pinax_state *state, uint32_t index, pinax_value *out);

/* After a run that ended with a fault: the locals of the word the fault happened in, as they
 * were then, with their names (code compiled from source carries them; a container without
 * local records gives none). `index` counts from 0; past the last, PINAX_ERR_NOT_FOUND. At
 * most 16 locals are kept, while their names, types and str bytes fit in 1 KB; a str value is
 * cut to its first 32 bytes. Valid until the next run that ends, with a fault or without. */
PINAX_API pinax_err pinax_fault_local(const pinax_state *state, uint32_t index, pinax_str *name, pinax_value *out);

/* ---- running ----------------------------------------------------------------------- */

enum {
    PINAX_FIND_NESTED = 1u << 0 /* the word will be run from inside a host word */
};
/* Looks a word up and checks that it has exactly this signature. A plain name is a word of the
 * first source loaded; "module.word" is a word of that module, exported or not, and
 * "module.test.name" one of its tests. A word that may yield cannot
 * be run from inside a host word (the C stack is in the way): with PINAX_FIND_NESTED such a
 * word is refused here with PINAX_ERR_SIGNATURE, and pinax_run() inside a host word accepts
 * only words found with that flag. */
PINAX_API pinax_err pinax_find(pinax_state *state, const char *name, const char *signature,
                               uint32_t flags, pinax_word *out);

/* The words of a state, one by one: index 0, 1, 2 ... until PINAX_ERR_NOT_FOUND. `module` is
 * empty for a source loaded under no module name; "module.name" (or the name alone, for the
 * first source) is what pinax_find() takes. Tests are listed too, as "test.name". The strings
 * stay valid as long as the words do. */
typedef struct pinax_word_info {
    pinax_word word;
    pinax_str  module, name, signature;
} pinax_word_info;
PINAX_API pinax_err pinax_word_at(const pinax_state *state, uint32_t index, pinax_word_info *out);

/* Runs a word. args->i/s/f are the inputs; results are written through ri/rs/rf and stay
 * valid until the next call on this state. PINAX_SUSPENDED and PINAX_WAITING are not failures.
 * From inside a host word the word runs to its end before pinax_run() returns: when the
 * instruction budget runs out there the answer is PINAX_ERR_INTERRUPTED, and the host word
 * should pass it on. */
PINAX_API pinax_err pinax_run(pinax_state *state, pinax_word word, const pinax_args *args);
/* Continues after PINAX_SUSPENDED, or after pinax_complete() following PINAX_WAITING. results is
 * where the outputs of the original pinax_run() land; pass the same pointers again. */
PINAX_API pinax_err pinax_resume(pinax_state *state, const pinax_args *results);
/* Gives up a run that is suspended (PINAX_SUSPENDED) or waiting (PINAX_WAITING), as a fault
 * would end it: the handles the run opened are closed, its tasks are freed, and pinax_complete()
 * answers PINAX_ERR_NOT_FOUND for its pending tokens. What it already stored in globals stays. A suspended load
 * is taken back as a failed one is. The state then takes the next pinax_run(). Answers
 * PINAX_ERR_STATE when nothing is suspended or a host word is running. */
PINAX_API pinax_err pinax_abort(pinax_state *state);
PINAX_API void      pinax_set_budget(pinax_state *state, uint64_t instructions);

/* ---- images and copies -------------------------------------------------------------- */

/* An image is the whole state: its code and declarations, globals, registers, region and
 * suspended runs and tasks; a head of 64 bytes and the used part of the block, as they are.
 * pinax_image_size() is its length. Saving is refused (PINAX_ERR_STATE) while a host word runs
 * or a load has not finished. */
PINAX_API size_t    pinax_image_size(const pinax_state *state);
PINAX_API pinax_err pinax_image_save(const pinax_state *state, pinax_mem out, size_t *written);
/* A state restored from an image. When `image` begins where `block` does (an image read or
 * mapped there, private and writable), it is opened in place without copying; the state then
 * lives in that memory and writes to it. A block that is not 16-byte aligned costs one move.
 * Otherwise the image is copied into the block. The block holds the image and what the state
 * will add (further loads); `config` gives the budget and diag, the rest is the image's.
 * Handles are closed (without their close functions: the objects were the other process's),
 * bound globals are back on their own storage, operations a run waited for come back
 * interrupted. Host words, handle types and extern functions are given again by name
 * (pinax_register, pinax_handle_type, pinax_set_resolver); bound globals are bound again.
 * An image of another format or another build's memory layout is PINAX_ERR_FORMAT. An image
 * is trusted like bound memory: it is not checked as bytecode is. */
PINAX_API pinax_err pinax_image_open(pinax_mem block, pinax_str image, const pinax_config *config,
                                     pinax_state **out);
/* A second state with the same code and data, in its own block: an image of the source saved
 * into the block and opened there. As after pinax_image_open(), host words, handle types,
 * extern functions and bound globals are given again; the source is not changed. */
PINAX_API pinax_err pinax_clone(const pinax_state *source, pinax_mem block, pinax_state **out);

/* ---- measuring --------------------------------------------------------------------- */

typedef struct pinax_stats {
    size_t   struct_size;
    uint32_t int_slots_peak, str_slots_peak, float_slots_peak;
    uint32_t region_bytes_peak;
    uint32_t handles_peak;
    uint32_t depth_peak;
    uint64_t instructions;
    size_t   block_bytes_used;
} pinax_stats;
PINAX_API void pinax_stats_get(const pinax_state *state, pinax_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* PINAX_H */

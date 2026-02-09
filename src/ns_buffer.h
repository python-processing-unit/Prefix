#ifndef NS_BUFFER_H
#define NS_BUFFER_H

#include "value.h"
#include "win32_shim.h"

struct Env; // forward declaration

// ---------- Operation types for namespace writes ----------

typedef enum {
    NS_OP_DEFINE,
    NS_OP_ASSIGN,
    NS_OP_DELETE,
    NS_OP_ALIAS,
    NS_OP_FREEZE,
    NS_OP_THAW,
    NS_OP_PERMAFREEZE
} NsOpType;

// A single write operation enqueued in the central buffer.
typedef struct NsOp {
    NsOpType op;
    struct Env* env;
    char* name;             // symbol name (owned copy)
    Value value;            // for ASSIGN
    DeclType decl_type;     // for DEFINE / ASSIGN / ALIAS
    bool declare_if_missing;// for ASSIGN / ALIAS
    char* target_name;      // for ALIAS  (owned copy)

    // Result fields – filled by the prepare thread after execution
    bool result_ok;         // true = success (for bool-returning ops)
    int  result_int;        // return value  (for int-returning ops)

    // Completion signalling
    bool completed;
    mtx_t completion_mtx;
    cnd_t completion_cnd;

    struct NsOp* next;
} NsOp;

// Per-symbol write queue ("symbol thread" – a logical thread).
typedef struct SymbolThread {
    char* name;             // symbol name (owned copy)
    NsOp* head;
    NsOp* tail;
    size_t pending_count;
    mtx_t lock;             // protects head / tail / pending_count
    cnd_t drain_cnd;        // signalled when pending_count reaches 0
    struct SymbolThread* next;
} SymbolThread;

// Central namespace buffer.
typedef struct NsBuffer {
    // Central write queue
    NsOp* queue_head;
    NsOp* queue_tail;
    mtx_t queue_mtx;
    cnd_t queue_cnd;        // signalled when a new op is enqueued or on shutdown

    // Symbol thread registry (linked list)
    SymbolThread* symbols;
    mtx_t symbols_mtx;

    // Global env-access mutex – held by the prepare thread during writes
    // and by readers during reads to prevent structural races (e.g. realloc).
    mtx_t env_mtx;

    // Prepare thread (hardware thread)
    thrd_t prepare_thrd;
    volatile int running;   // using int for volatile-safe reads
} NsBuffer;

// ---------- Public API ----------

// Initialise the global namespace buffer and start the prepare thread.
// Must be called once before any env_* function that should be buffered.
void ns_buffer_init(void);

// Shut down the buffer system, drain remaining operations, and join
// the prepare thread.  After this call, env_* functions revert to
// direct (unbuffered) execution.
void ns_buffer_shutdown(void);

// Returns true if the buffer system is active.
bool ns_buffer_active(void);

// Block the calling thread until all pending writes for `name` have
// been processed.  Then acquire the env-access lock so the caller can
// safely read.  The caller MUST call ns_buffer_read_unlock() when done.
void ns_buffer_read_lock(const char* name);

// Release the env-access lock acquired by ns_buffer_read_lock().
void ns_buffer_read_unlock(void);

// ---------- Buffered write entry points ----------
// Each function enqueues the operation, blocks until completion, and
// returns the result.

bool ns_buffer_define(struct Env* env, const char* name, DeclType type);
bool ns_buffer_assign(struct Env* env, const char* name, Value value,
                      DeclType type, bool declare_if_missing);
bool ns_buffer_delete(struct Env* env, const char* name);
bool ns_buffer_set_alias(struct Env* env, const char* name,
                         const char* target_name, DeclType type,
                         bool declare_if_missing);
int  ns_buffer_freeze(struct Env* env, const char* name);
int  ns_buffer_thaw(struct Env* env, const char* name);
int  ns_buffer_permafreeze(struct Env* env, const char* name);

#endif // NS_BUFFER_H

/*
 * ns_buffer.c – Centralised namespace write-buffer implementation.
 *
 * Architecture (per gh-15):
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │                   Central Write Buffer                   │
 *   │  (thread-safe FIFO queue, fed by any interpreter thread) │
 *   └────────────────────────────┬─────────────────────────────┘
 *                                │  dequeued by
 *                                ▼
 *                        ┌───────────────┐
 *                        │ Prepare Thread│  (single hardware thread)
 *                        └───┬───┬───┬───┘
 *                            │   │   │     dispatches to
 *                ┌───────────┘   │   └───────────┐
 *                ▼               ▼               ▼
 *      ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
 *      │ Symbol Thread │ │ Symbol Thread │ │ Symbol Thread │  (logical)
 *      │      "x"      │ │      "y"      │ │      "z"      │
 *      └───────────────┘ └───────────────┘ └───────────────┘
 *
 * Read requests block until the queried symbol's buffer is empty,
 * then acquire a global env-access mutex for safe reading.
 */

#include "ns_buffer.h"
#include "env.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* ------------------------------------------------------------------ */
/*  Global singleton                                                   */
/* ------------------------------------------------------------------ */

static NsBuffer* g_ns_buf = NULL;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void* ns_alloc(size_t size) {
    void* p = malloc(size);
    if (!p) { fprintf(stderr, "ns_buffer: out of memory\n"); exit(1); }
    memset(p, 0, size);
    return p;
}

/* Look up a SymbolThread by name.  Must be called with symbols_mtx held. */
static SymbolThread* find_symbol_thread(NsBuffer* buf, const char* name) {
    for (SymbolThread* st = buf->symbols; st; st = st->next)
        if (strcmp(st->name, name) == 0) return st;
    return NULL;
}

/* Find or create a SymbolThread.  Must be called with symbols_mtx held. */
static SymbolThread* find_or_create_symbol_thread(NsBuffer* buf, const char* name) {
    SymbolThread* st = find_symbol_thread(buf, name);
    if (st) return st;

    st = ns_alloc(sizeof(SymbolThread));
    st->name = strdup(name);
    st->head = st->tail = NULL;
    st->pending_count = 0;
    mtx_init(&st->lock, 0);
    cnd_init(&st->drain_cnd);
    st->next = buf->symbols;
    buf->symbols = st;
    return st;
}

/* ------------------------------------------------------------------ */
/*  Direct (unbuffered) env_* functions – declared in env.h            */
/* ------------------------------------------------------------------ */

/* These are defined in env.c and have the prefix env_*_direct.        */

/* ------------------------------------------------------------------ */
/*  Execute a single NsOp against the real environment.                */
/*  Called on the prepare thread while holding buf->env_mtx.           */
/* ------------------------------------------------------------------ */

static void execute_op(NsOp* op) {
    switch (op->op) {
    case NS_OP_DEFINE:
        op->result_ok = env_define_direct(op->env, op->name, op->decl_type);
        break;
    case NS_OP_ASSIGN:
        op->result_ok = env_assign_direct(op->env, op->name, op->value,
                                          op->decl_type, op->declare_if_missing);
        break;
    case NS_OP_DELETE:
        op->result_ok = env_delete_direct(op->env, op->name);
        break;
    case NS_OP_ALIAS:
        op->result_ok = env_set_alias_direct(op->env, op->name,
                                             op->target_name, op->decl_type,
                                             op->declare_if_missing);
        break;
    case NS_OP_FREEZE:
        op->result_int = env_freeze_direct(op->env, op->name);
        break;
    case NS_OP_THAW:
        op->result_int = env_thaw_direct(op->env, op->name);
        break;
    case NS_OP_PERMAFREEZE:
        op->result_int = env_permafreeze_direct(op->env, op->name);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Prepare-thread main loop                                           */
/* ------------------------------------------------------------------ */

static int prepare_thread_func(void* arg) {
    NsBuffer* buf = (NsBuffer*)arg;

    while (buf->running) {
        /* ---- Phase 1: wait for and dequeue the oldest operation ---- */
        mtx_lock(&buf->queue_mtx);
        while (!buf->queue_head && buf->running)
            cnd_wait(&buf->queue_cnd, &buf->queue_mtx);

        if (!buf->running && !buf->queue_head) {
            mtx_unlock(&buf->queue_mtx);
            break;
        }

        NsOp* op = buf->queue_head;
        buf->queue_head = op->next;
        if (!buf->queue_head) buf->queue_tail = NULL;
        op->next = NULL;
        mtx_unlock(&buf->queue_mtx);

        /* ---- Phase 2: dispatch to the appropriate symbol thread ---- */
        mtx_lock(&buf->symbols_mtx);
        SymbolThread* st = find_or_create_symbol_thread(buf, op->name);
        mtx_unlock(&buf->symbols_mtx);

        mtx_lock(&st->lock);
        if (st->tail) st->tail->next = op;
        else          st->head = op;
        st->tail = op;
        st->pending_count++;
        mtx_unlock(&st->lock);

        /* ---- Phase 3: execute (as the symbol's logical thread) ---- */
        mtx_lock(&buf->env_mtx);
        execute_op(op);
        mtx_unlock(&buf->env_mtx);

        /* ---- Phase 4: remove from symbol thread & signal drain ---- */
        mtx_lock(&st->lock);
        st->head = op->next;
        if (!st->head) st->tail = NULL;
        st->pending_count--;
        if (st->pending_count == 0)
            cnd_broadcast(&st->drain_cnd);
        mtx_unlock(&st->lock);

        /* ---- Phase 5: signal the waiting writer ---- */
        mtx_lock(&op->completion_mtx);
        op->completed = true;
        cnd_signal(&op->completion_cnd);
        mtx_unlock(&op->completion_mtx);
    }

    /* ---- Drain any remaining operations on shutdown ---- */
    mtx_lock(&buf->queue_mtx);
    while (buf->queue_head) {
        NsOp* op = buf->queue_head;
        buf->queue_head = op->next;
        op->next = NULL;
        mtx_unlock(&buf->queue_mtx);

        mtx_lock(&buf->symbols_mtx);
        SymbolThread* st = find_or_create_symbol_thread(buf, op->name);
        mtx_unlock(&buf->symbols_mtx);

        mtx_lock(&buf->env_mtx);
        execute_op(op);
        mtx_unlock(&buf->env_mtx);

        mtx_lock(&st->lock);
        if (st->pending_count > 0) st->pending_count--;
        if (st->pending_count == 0) cnd_broadcast(&st->drain_cnd);
        mtx_unlock(&st->lock);

        mtx_lock(&op->completion_mtx);
        op->completed = true;
        cnd_signal(&op->completion_cnd);
        mtx_unlock(&op->completion_mtx);

        mtx_lock(&buf->queue_mtx);
    }
    mtx_unlock(&buf->queue_mtx);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public: lifecycle                                                  */
/* ------------------------------------------------------------------ */

void ns_buffer_init(void) {
    if (g_ns_buf) return; /* already initialised */

    NsBuffer* buf = ns_alloc(sizeof(NsBuffer));
    buf->queue_head = buf->queue_tail = NULL;
    mtx_init(&buf->queue_mtx, 0);
    cnd_init(&buf->queue_cnd);

    buf->symbols = NULL;
    mtx_init(&buf->symbols_mtx, 0);

    mtx_init(&buf->env_mtx, 0);

    buf->running = 1;

    if (thrd_create(&buf->prepare_thrd, prepare_thread_func, buf) != thrd_success) {
        fprintf(stderr, "ns_buffer: failed to create prepare thread\n");
        exit(1);
    }

    g_ns_buf = buf;
}

void ns_buffer_shutdown(void) {
    if (!g_ns_buf) return;
    NsBuffer* buf = g_ns_buf;

    /* Signal the prepare thread to stop */
    buf->running = 0;
    mtx_lock(&buf->queue_mtx);
    cnd_signal(&buf->queue_cnd);
    mtx_unlock(&buf->queue_mtx);

    thrd_join(buf->prepare_thrd, NULL);

    /* Free symbol threads */
    SymbolThread* st = buf->symbols;
    while (st) {
        SymbolThread* next = st->next;
        free(st->name);
        mtx_destroy(&st->lock);
        cnd_destroy(&st->drain_cnd);
        free(st);
        st = next;
    }

    mtx_destroy(&buf->queue_mtx);
    cnd_destroy(&buf->queue_cnd);
    mtx_destroy(&buf->symbols_mtx);
    mtx_destroy(&buf->env_mtx);
    free(buf);
    g_ns_buf = NULL;
}

bool ns_buffer_active(void) {
    return g_ns_buf != NULL && g_ns_buf->running;
}

/* ------------------------------------------------------------------ */
/*  Public: read-side synchronisation                                  */
/* ------------------------------------------------------------------ */

void ns_buffer_read_lock(const char* name) {
    if (!g_ns_buf || !g_ns_buf->running) return;
    NsBuffer* buf = g_ns_buf;

    /* Wait until all pending writes for this symbol are drained */
    mtx_lock(&buf->symbols_mtx);
    SymbolThread* st = find_symbol_thread(buf, name);
    mtx_unlock(&buf->symbols_mtx);

    if (st) {
        mtx_lock(&st->lock);
        while (st->pending_count > 0)
            cnd_wait(&st->drain_cnd, &st->lock);
        mtx_unlock(&st->lock);
    }

    /* Acquire the global env-access lock so the reader is safe from
       concurrent structural mutations (e.g. realloc in env_define). */
    mtx_lock(&buf->env_mtx);
}

void ns_buffer_read_unlock(void) {
    if (!g_ns_buf) return;
    mtx_unlock(&g_ns_buf->env_mtx);
}

/* ------------------------------------------------------------------ */
/*  Internal: enqueue an operation and wait for completion             */
/* ------------------------------------------------------------------ */

static NsOp* make_op(NsOpType type, struct Env* env, const char* name) {
    NsOp* op = ns_alloc(sizeof(NsOp));
    op->op = type;
    op->env = env;
    op->name = strdup(name);
    op->completed = false;
    mtx_init(&op->completion_mtx, 0);
    cnd_init(&op->completion_cnd);
    return op;
}

static void enqueue_op(NsOp* op) {
    NsBuffer* buf = g_ns_buf;
    mtx_lock(&buf->queue_mtx);
    if (buf->queue_tail) buf->queue_tail->next = op;
    else                 buf->queue_head = op;
    buf->queue_tail = op;
    cnd_signal(&buf->queue_cnd);
    mtx_unlock(&buf->queue_mtx);
}

static void wait_op(NsOp* op) {
    mtx_lock(&op->completion_mtx);
    while (!op->completed)
        cnd_wait(&op->completion_cnd, &op->completion_mtx);
    mtx_unlock(&op->completion_mtx);
}

static void free_op(NsOp* op) {
    free(op->name);
    if (op->target_name) free(op->target_name);
    /* Note: op->value is NOT freed here – ownership was transferred
       to the env by the _direct function.  If the op failed we must
       still not double-free because the _direct function already
       consumed (or left untouched) the value copy we made. */
    mtx_destroy(&op->completion_mtx);
    cnd_destroy(&op->completion_cnd);
    free(op);
}

/* ------------------------------------------------------------------ */
/*  Public: buffered write entry points                                */
/* ------------------------------------------------------------------ */

bool ns_buffer_define(struct Env* env, const char* name, DeclType type) {
    NsOp* op = make_op(NS_OP_DEFINE, env, name);
    op->decl_type = type;
    enqueue_op(op);
    wait_op(op);
    bool r = op->result_ok;
    free_op(op);
    return r;
}

bool ns_buffer_assign(struct Env* env, const char* name, Value value,
                      DeclType type, bool declare_if_missing) {
    NsOp* op = make_op(NS_OP_ASSIGN, env, name);
    op->value = value_copy(value);   /* transfer a copy into the op */
    op->decl_type = type;
    op->declare_if_missing = declare_if_missing;
    enqueue_op(op);
    wait_op(op);
    bool r = op->result_ok;
    /* Free the value copy we made – env_assign_direct did its own copy */
    value_free(op->value);
    free_op(op);
    return r;
}

bool ns_buffer_delete(struct Env* env, const char* name) {
    NsOp* op = make_op(NS_OP_DELETE, env, name);
    enqueue_op(op);
    wait_op(op);
    bool r = op->result_ok;
    free_op(op);
    return r;
}

bool ns_buffer_set_alias(struct Env* env, const char* name,
                         const char* target_name, DeclType type,
                         bool declare_if_missing) {
    NsOp* op = make_op(NS_OP_ALIAS, env, name);
    op->target_name = strdup(target_name);
    op->decl_type = type;
    op->declare_if_missing = declare_if_missing;
    enqueue_op(op);
    wait_op(op);
    bool r = op->result_ok;
    free_op(op);
    return r;
}

int ns_buffer_freeze(struct Env* env, const char* name) {
    NsOp* op = make_op(NS_OP_FREEZE, env, name);
    enqueue_op(op);
    wait_op(op);
    int r = op->result_int;
    free_op(op);
    return r;
}

int ns_buffer_thaw(struct Env* env, const char* name) {
    NsOp* op = make_op(NS_OP_THAW, env, name);
    enqueue_op(op);
    wait_op(op);
    int r = op->result_int;
    free_op(op);
    return r;
}

int ns_buffer_permafreeze(struct Env* env, const char* name) {
    NsOp* op = make_op(NS_OP_PERMAFREEZE, env, name);
    enqueue_op(op);
    wait_op(op);
    int r = op->result_int;
    free_op(op);
    return r;
}

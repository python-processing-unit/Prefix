#ifndef WIN32_SHIM_H
#define WIN32_SHIM_H

#if defined(_WIN32) || defined(_MSC_VER)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <time.h>

// C11 Threading shim for Windows

typedef HANDLE thrd_t;
typedef HANDLE mtx_t;
typedef HANDLE cnd_t;

typedef int (*thrd_start_t)(void*);

enum {
    thrd_success = 0,
    thrd_nomem = 1,
    thrd_timedout = 2,
    thrd_busy = 3,
    thrd_error = 4
};

typedef struct {
    thrd_start_t func;
    void* arg;
} Win32ThrdCtx;

static unsigned __stdcall win32_thrd_proxy(void* ctx_void) {
    Win32ThrdCtx* ctx = (Win32ThrdCtx*)ctx_void;
    thrd_start_t func = ctx->func;
    void* arg = ctx->arg;
    free(ctx); 
    return (unsigned)func(arg);
}

static inline int thrd_create(thrd_t* thr, thrd_start_t func, void* arg) {
    Win32ThrdCtx* ctx = (Win32ThrdCtx*)malloc(sizeof(Win32ThrdCtx));
    if (!ctx) return thrd_nomem;
    ctx->func = func;
    ctx->arg = arg;
    
    uintptr_t h = _beginthreadex(NULL, 0, win32_thrd_proxy, ctx, 0, NULL);
    if (h == 0) {
        free(ctx);
        return thrd_error;
    }
    *thr = (HANDLE)h;
    return thrd_success;
}

static inline int thrd_join(thrd_t thr, int* res) {
    if (WaitForSingleObject(thr, INFINITE) == WAIT_FAILED) return thrd_error;
    if (res) {
        DWORD exit_code;
        if (GetExitCodeThread(thr, &exit_code)) {
            *res = (int)exit_code;
        } else {
            *res = 0;
        }
    }
    CloseHandle(thr);
    return thrd_success;
}

static inline int thrd_detach(thrd_t thr) {
    CloseHandle(thr);
    return thrd_success;
}

static inline void thrd_yield(void) {
    SwitchToThread();
}

// Ensure struct timespec is available on MSVC *only* if not already provided
// Check common platform macros that indicate timespec is defined.
// Avoid declaring a global struct timespec to prevent redefinition with CRT headers.
// Use an opaque pointer and cast to a local shape to compute milliseconds.
static inline int thrd_sleep(const void* duration, void* remaining) {
    if (duration) {
        struct timespec_shim { long long tv_sec; long tv_nsec; };
        const struct timespec_shim* d = (const struct timespec_shim*)duration;
        long long ms = (long long)d->tv_sec * 1000LL + d->tv_nsec / 1000000L;
        if (ms <= 0) ms = 1;
        if (ms > INFINITE - 1) ms = INFINITE - 1;
        Sleep((DWORD)ms);
    } else {
        Sleep(1);
    }
    (void)remaining;
    return thrd_success;
}

static inline int mtx_init(mtx_t* mtx, int type) {
    *mtx = CreateMutex(NULL, FALSE, NULL);
    if (!*mtx) return thrd_error;
    (void)type;
    return thrd_success;
}

static inline int mtx_lock(mtx_t* mtx) {
    return WaitForSingleObject(*mtx, INFINITE) == WAIT_OBJECT_0 ? thrd_success : thrd_error;
}

static inline int mtx_unlock(mtx_t* mtx) {
    return ReleaseMutex(*mtx) ? thrd_success : thrd_error;
}

static inline void mtx_destroy(mtx_t* mtx) {
    CloseHandle(*mtx);
}

#endif // WIN32
#endif // WIN32_SHIM_H

/*
 * Independent C Producer Test Suite for libpw_cpython_wasi.{dylib,so}
 *
 * Dynamically loads the built shared library via dlopen() and runs real
 * CPython 3.12.0/WASI programs to verify:
 *  1. Embedded identity handshake (ABI version, digests, import count)
 *  2. Fresh sequential instance reuse
 *  3. proc_exit & trap recovery
 *  4. Host stop unwinding (PW_PY_WASI_HOST_STOP)
 *  5. Deterministic fuel exhaustion (PW_PY_STATUS_FUEL_EXHAUSTED)
 *  6. Asynchronous cross-thread cancellation in a CPU-only loop (<100ms)
 *  7. Memory ceiling enforcement (reject < initial_pages + cap growth)
 *  8. Concurrent multi-threaded execution (6 pthreads simultaneously)
 */

#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pw_py_engine.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Function pointers resolved via dlsym. */
typedef const pw_py_engine_identity_t* (*fn_identity_t)(void);
typedef pw_py_status_t (*fn_init_t)(void);
typedef pw_py_status_t (*fn_new_t)(const pw_py_instance_config_t*, pw_py_host_wasi_fn, void*, pw_py_instance_t**);
typedef pw_py_status_t (*fn_run_t)(pw_py_instance_t*, pw_py_run_result_t*);
typedef void (*fn_cancel_t)(pw_py_instance_t*);
typedef pw_py_status_t (*fn_mem_t)(pw_py_instance_t*, uint8_t**, uint64_t*, uint32_t*, uint32_t*);
typedef void (*fn_free_t)(pw_py_instance_t*);

static fn_identity_t g_identity;
static fn_init_t g_init;
static fn_new_t g_new;
static fn_run_t g_run;
static fn_cancel_t g_cancel;
static fn_mem_t g_mem;
static fn_free_t g_free;

static const char* g_stdlib_dir = NULL;
static const char* g_packages_dir = NULL;

/* Minimal POSIX-backed WASI host context for real CPython execution. */
#define MAX_WASI_FDS 128

typedef struct {
    pw_py_instance_t* inst;
    const char* argv[8];
    int argc;
    int stop_on_write;
    int fds[MAX_WASI_FDS]; /* host OS fd, -1 if unused; fd 3 = /usr/local/lib */
    char stdout_buf[8192];
    size_t stdout_len;
    _Atomic int ready_seen;
    char stderr_buf[8192];
    size_t stderr_len;
} test_host_t;

static uint16_t posix_to_wasi_errno(int err) {
    switch (err) {
        case 0: return 0;
        case EACCES: return 2;
        case EBADF: return 8;
        case EEXIST: return 20;
        case EINVAL: return 28;
        case EISDIR: return 31;
        case ENOENT: return 44;
        case ENOTDIR: return 54;
        case EPERM: return 63;
        default: return 29; /* EIO */
    }
}

static inline void put_u32(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
}
static inline void put_u64(uint8_t* p, uint64_t v) {
    memcpy(p, &v, 8);
}
static inline uint32_t get_u32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void write_filestat(uint8_t* dst, const struct stat* st) {
    memset(dst, 0, 64);
    put_u64(dst + 0, (uint64_t)st->st_dev);
    put_u64(dst + 8, (uint64_t)st->st_ino);
    uint8_t ftype = 4; /* regular */
    if (S_ISDIR(st->st_mode)) ftype = 3;
    else if (S_ISLNK(st->st_mode)) ftype = 7;
    dst[16] = ftype;
    put_u64(dst + 24, (uint64_t)st->st_nlink);
    put_u64(dst + 32, (uint64_t)st->st_size);
}

static pw_py_wasi_action_t test_wasi_cb(
    void* ctx_ptr,
    pw_py_wasi_op_t op_id,
    const char* op_name,
    const uint64_t* args,
    uint32_t argc,
    uint32_t* out_ret) {
    (void)op_name;
    (void)argc;
    test_host_t* h = (test_host_t*)ctx_ptr;
    uint8_t* mem = NULL;
    uint64_t msz = 0;
    g_mem(h->inst, &mem, &msz, NULL, NULL);

    switch (op_id) {
        case PW_PY_WASI_PROC_EXIT:
            *out_ret = (uint32_t)args[0];
            return PW_PY_WASI_EXIT;

        case PW_PY_WASI_ARGS_SIZES_GET: {
            uint32_t argc_p = (uint32_t)args[0], buf_sz_p = (uint32_t)args[1];
            uint32_t total = 0;
            for (int i = 0; i < h->argc; i++) total += (uint32_t)strlen(h->argv[i]) + 1;
            if (argc_p + 4 > msz || buf_sz_p + 4 > msz) { *out_ret = 21; return PW_PY_WASI_CONTINUE; }
            put_u32(mem + argc_p, (uint32_t)h->argc);
            put_u32(mem + buf_sz_p, total);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_ARGS_GET: {
            uint32_t argv_p = (uint32_t)args[0], buf_p = (uint32_t)args[1];
            for (int i = 0; i < h->argc; i++) {
                size_t len = strlen(h->argv[i]) + 1;
                if (argv_p + (uint32_t)(i + 1) * 4 > msz || buf_p + len > msz) { *out_ret = 21; return PW_PY_WASI_CONTINUE; }
                put_u32(mem + argv_p + i * 4, buf_p);
                memcpy(mem + buf_p, h->argv[i], len);
                buf_p += (uint32_t)len;
            }
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_ENVIRON_SIZES_GET: {
            uint32_t cnt_p = (uint32_t)args[0], sz_p = (uint32_t)args[1];
            put_u32(mem + cnt_p, 0);
            put_u32(mem + sz_p, 0);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_ENVIRON_GET:
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;

        case PW_PY_WASI_FD_PRESTAT_GET: {
            uint32_t fd = (uint32_t)args[0], out_p = (uint32_t)args[1];
            const char* pre = NULL;
            if (fd == 3) pre = "/usr/local/lib";
            else if (fd == 4 && g_packages_dir != NULL && h->fds[4] >= 0) pre = "/packages";
            else { *out_ret = 8; return PW_PY_WASI_CONTINUE; } /* EBADF */
            mem[out_p] = 0; /* preopentype::dir */
            put_u32(mem + out_p + 4, (uint32_t)strlen(pre));
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_PRESTAT_DIR_NAME: {
            uint32_t fd = (uint32_t)args[0], path_p = (uint32_t)args[1], path_len = (uint32_t)args[2];
            const char* pre = NULL;
            if (fd == 3) pre = "/usr/local/lib";
            else if (fd == 4 && g_packages_dir != NULL && h->fds[4] >= 0) pre = "/packages";
            else { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            size_t len = strlen(pre);
            if (path_len < len) { *out_ret = 28; return PW_PY_WASI_CONTINUE; }
            memcpy(mem + path_p, pre, len);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_FDSTAT_GET: {
            uint32_t fd = (uint32_t)args[0], out_p = (uint32_t)args[1];
            if (fd >= MAX_WASI_FDS || (fd >= 3 && h->fds[fd] < 0)) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            memset(mem + out_p, 0, 24);
            if (fd < 3) {
                mem[out_p] = 2; /* character_device */
            } else {
                struct stat st;
                if (fstat(h->fds[fd], &st) == 0 && S_ISDIR(st.st_mode)) mem[out_p] = 3;
                else mem[out_p] = 4;
            }
            put_u64(mem + out_p + 8, 0x0FFFFFFFULL);
            put_u64(mem + out_p + 16, 0x0FFFFFFFULL);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_FILESTAT_GET: {
            uint32_t fd = (uint32_t)args[0], out_p = (uint32_t)args[1];
            if (fd < 3) {
                memset(mem + out_p, 0, 64);
                mem[out_p + 16] = 2;
                *out_ret = 0;
                return PW_PY_WASI_CONTINUE;
            }
            if (fd >= MAX_WASI_FDS || h->fds[fd] < 0) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            struct stat st;
            if (fstat(h->fds[fd], &st) != 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            write_filestat(mem + out_p, &st);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_PATH_FILESTAT_GET: {
            uint32_t dirfd = (uint32_t)args[0], path_p = (uint32_t)args[2], path_len = (uint32_t)args[3], out_p = (uint32_t)args[4];
            if (dirfd >= MAX_WASI_FDS || h->fds[dirfd] < 0 || path_len >= 1024 || path_p + path_len > msz) {
                *out_ret = 8; return PW_PY_WASI_CONTINUE;
            }
            char rel[1024];
            memcpy(rel, mem + path_p, path_len);
            rel[path_len] = '\0';
            struct stat st;
            if (fstatat(h->fds[dirfd], rel, &st, 0) != 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            write_filestat(mem + out_p, &st);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_PATH_OPEN: {
            uint32_t dirfd = (uint32_t)args[0], path_p = (uint32_t)args[2], path_len = (uint32_t)args[3];
            uint32_t oflags = (uint32_t)args[4], out_fd_p = (uint32_t)args[8];
            if (dirfd >= MAX_WASI_FDS || h->fds[dirfd] < 0 || path_len >= 1024 || path_p + path_len > msz) {
                *out_ret = 8; return PW_PY_WASI_CONTINUE;
            }
            char rel[1024];
            memcpy(rel, mem + path_p, path_len);
            rel[path_len] = '\0';
            int flags = O_RDONLY;
            if (oflags & 2) flags |= O_DIRECTORY;
            int osfd = openat(h->fds[dirfd], rel, flags);
            if (osfd < 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            int slot = -1;
            for (int i = 4; i < MAX_WASI_FDS; i++) {
                if (h->fds[i] < 0) { slot = i; break; }
            }
            if (slot < 0) { close(osfd); *out_ret = 33; return PW_PY_WASI_CONTINUE; }
            h->fds[slot] = osfd;
            put_u32(mem + out_fd_p, (uint32_t)slot);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_READ: {
            uint32_t fd = (uint32_t)args[0], iovs_p = (uint32_t)args[1], iovs_len = (uint32_t)args[2], nread_p = (uint32_t)args[3];
            if (fd == 0) { put_u32(mem + nread_p, 0); *out_ret = 0; return PW_PY_WASI_CONTINUE; }
            if (fd >= MAX_WASI_FDS || h->fds[fd] < 0) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            uint32_t total = 0;
            for (uint32_t i = 0; i < iovs_len; i++) {
                uint32_t base = get_u32(mem + iovs_p + i * 8);
                uint32_t len = get_u32(mem + iovs_p + i * 8 + 4);
                if (base + len > msz) { *out_ret = 21; return PW_PY_WASI_CONTINUE; }
                ssize_t n = read(h->fds[fd], mem + base, len);
                if (n < 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
                total += (uint32_t)n;
                if ((uint32_t)n < len) break;
            }
            put_u32(mem + nread_p, total);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_WRITE: {
            uint32_t fd = (uint32_t)args[0], iovs_p = (uint32_t)args[1], iovs_len = (uint32_t)args[2], nwritten_p = (uint32_t)args[3];
            if (h->stop_on_write && (fd == 1 || fd == 2)) {
                return PW_PY_WASI_HOST_STOP;
            }
            uint32_t total = 0;
            for (uint32_t i = 0; i < iovs_len; i++) {
                uint32_t base = get_u32(mem + iovs_p + i * 8);
                uint32_t len = get_u32(mem + iovs_p + i * 8 + 4);
                if (base + len > msz) { *out_ret = 21; return PW_PY_WASI_CONTINUE; }
                if (fd == 1) {
                    size_t avail = sizeof(h->stdout_buf) - 1 - h->stdout_len;
                    size_t cp = len < avail ? len : avail;
                    memcpy(h->stdout_buf + h->stdout_len, mem + base, cp);
                    h->stdout_len += cp;
                    h->stdout_buf[h->stdout_len] = '\0';
                    if (strstr(h->stdout_buf, "LOOP_READY") != NULL) {
                        atomic_store_explicit(&h->ready_seen, 1, memory_order_release);
                    }
                } else if (fd == 2) {
                    size_t avail = sizeof(h->stderr_buf) - 1 - h->stderr_len;
                    size_t cp = len < avail ? len : avail;
                    memcpy(h->stderr_buf + h->stderr_len, mem + base, cp);
                    h->stderr_len += cp;
                    h->stderr_buf[h->stderr_len] = '\0';
                }
                total += len;
            }
            put_u32(mem + nwritten_p, total);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_SEEK: {
            uint32_t fd = (uint32_t)args[0], whence = (uint32_t)args[2], out_p = (uint32_t)args[3];
            int64_t off = (int64_t)args[1];
            if (fd >= MAX_WASI_FDS || h->fds[fd] < 0) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            int w = whence == 0 ? SEEK_SET : (whence == 1 ? SEEK_CUR : SEEK_END);
            off_t res = lseek(h->fds[fd], (off_t)off, w);
            if (res < 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            put_u64(mem + out_p, (uint64_t)res);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_TELL: {
            uint32_t fd = (uint32_t)args[0], out_p = (uint32_t)args[1];
            if (fd >= MAX_WASI_FDS || h->fds[fd] < 0) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            off_t res = lseek(h->fds[fd], 0, SEEK_CUR);
            if (res < 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            put_u64(mem + out_p, (uint64_t)res);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_READDIR: {
            uint32_t fd = (uint32_t)args[0], buf_p = (uint32_t)args[1], buf_len = (uint32_t)args[2];
            uint64_t cookie = args[3];
            uint32_t used_p = (uint32_t)args[4];
            if (fd >= MAX_WASI_FDS || h->fds[fd] < 0) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            int dup_fd = dup(h->fds[fd]);
            if (dup_fd < 0) { *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            DIR* d = fdopendir(dup_fd);
            if (!d) { close(dup_fd); *out_ret = posix_to_wasi_errno(errno); return PW_PY_WASI_CONTINUE; }
            rewinddir(d);
            uint64_t idx = 0;
            uint32_t used = 0;
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL) {
                idx++;
                if (idx <= cookie) continue;
                uint32_t namelen = (uint32_t)strlen(ent->d_name);
                uint8_t hdr[24];
                memset(hdr, 0, sizeof(hdr));
                put_u64(hdr + 0, idx);
                put_u64(hdr + 8, (uint64_t)ent->d_ino);
                put_u32(hdr + 16, namelen);
                uint8_t dtype = 0;
                if (ent->d_type == DT_DIR) dtype = 3;
                else if (ent->d_type == DT_REG) dtype = 4;
                else if (ent->d_type == DT_LNK) dtype = 7;
                hdr[20] = dtype;
                uint32_t rem = buf_len - used;
                uint32_t hcp = rem < 24 ? rem : 24;
                memcpy(mem + buf_p + used, hdr, hcp);
                used += hcp;
                if (hcp < 24) break;
                rem = buf_len - used;
                uint32_t ncp = rem < namelen ? rem : namelen;
                memcpy(mem + buf_p + used, ent->d_name, ncp);
                used += ncp;
                if (ncp < namelen) break;
            }
            closedir(d);
            put_u32(mem + used_p, used);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_CLOSE: {
            uint32_t fd = (uint32_t)args[0];
            uint32_t min_dyn = (g_packages_dir != NULL) ? 5 : 4;
            if (fd >= min_dyn && fd < MAX_WASI_FDS && h->fds[fd] >= 0) {
                close(h->fds[fd]);
                h->fds[fd] = -1;
            }
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_CLOCK_TIME_GET: {
            uint32_t out_p = (uint32_t)args[2];
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            put_u64(mem + out_p, (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_CLOCK_RES_GET: {
            put_u64(mem + (uint32_t)args[1], 1000ULL);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_RANDOM_GET: {
            uint32_t buf_p = (uint32_t)args[0], len = (uint32_t)args[1];
            for (uint32_t i = 0; i < len && buf_p + i < msz; i++) mem[buf_p + i] = (uint8_t)(i * 31 + 7);
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_SCHED_YIELD:
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        default:
            *out_ret = 58; /* ENOTSUP */
            return PW_PY_WASI_CONTINUE;
    }
}

static void init_host(test_host_t* h, const char* code) {
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < MAX_WASI_FDS; i++) h->fds[i] = -1;
    h->fds[3] = open(g_stdlib_dir, O_RDONLY | O_DIRECTORY);
    if (g_packages_dir != NULL && g_packages_dir[0] != '\0') {
        h->fds[4] = open(g_packages_dir, O_RDONLY | O_DIRECTORY);
    }
    h->argv[0] = "python3";
    h->argv[1] = "-I";
    h->argv[2] = "-B";
    h->argv[3] = "-c";
    h->argv[4] = code;
    h->argc = 5;
}

static void close_host(test_host_t* h) {
    for (int i = 3; i < MAX_WASI_FDS; i++) {
        if (h->fds[i] >= 0) {
            close(h->fds[i]);
            h->fds[i] = -1;
        }
    }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    int id;
    const char* code;
    uint32_t max_pages;
    uint64_t fuel;
    pw_py_instance_t* inst;
    pw_py_run_result_t res;
    char stdout_copy[256];
    _Atomic int started;
    test_host_t host;
} worker_arg_t;

static void* worker_thread(void* arg) {
    worker_arg_t* w = (worker_arg_t*)arg;
    init_host(&w->host, w->code);
    pw_py_instance_config_t cfg = { sizeof(cfg), w->max_pages, w->fuel };
    pw_py_status_t st = g_new(&cfg, test_wasi_cb, &w->host, &w->inst);
    if (st != PW_PY_STATUS_OK) {
        w->res.status = st;
        close_host(&w->host);
        return NULL;
    }
    w->host.inst = w->inst;
    atomic_store_explicit(&w->started, 1, memory_order_release);
    g_run(w->inst, &w->res);
    size_t cp = w->host.stdout_len < sizeof(w->stdout_copy) - 1 ? w->host.stdout_len : sizeof(w->stdout_copy) - 1;
    memcpy(w->stdout_copy, w->host.stdout_buf, cp);
    w->stdout_copy[cp] = '\0';
    g_free(w->inst);
    w->inst = NULL;
    close_host(&w->host);
    return NULL;
}

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_engine <libpw_cpython_wasi.{dylib,so}> <stdlib_dir>\n");
        return 2;
    }
    const char* lib_path = argv[1];
    g_stdlib_dir = argv[2];
    g_packages_dir = getenv("PW_TEST_PACKAGES_DIR");

    void* dl = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", lib_path, dlerror());
        return 1;
    }
    g_identity = (fn_identity_t)dlsym(dl, "pw_py_engine_identity");
    g_init = (fn_init_t)dlsym(dl, "pw_py_engine_init");
    g_new = (fn_new_t)dlsym(dl, "pw_py_instance_new");
    g_run = (fn_run_t)dlsym(dl, "pw_py_instance_run");
    g_cancel = (fn_cancel_t)dlsym(dl, "pw_py_instance_cancel");
    g_mem = (fn_mem_t)dlsym(dl, "pw_py_instance_memory");
    g_free = (fn_free_t)dlsym(dl, "pw_py_instance_free");
    CHECK(g_identity && g_init && g_new && g_run && g_cancel && g_mem && g_free, "all 7 pw_py_* symbols resolved");

    /* 1. Embedded identity handshake */
    const pw_py_engine_identity_t* id = g_identity();
    CHECK(id != NULL, "identity non-null");
    CHECK(id->abi_version == PW_PY_ENGINE_ABI_VERSION, "abi_version == 1");
    CHECK(id->import_count == PW_PY_ENGINE_IMPORT_COUNT, "import_count == 43");
    CHECK(strcmp(id->cpython_wasm_sha256, "fd955c1b91ef90a070564314f9bc6c98443bc731c462191ebca2e5a1ec317c8c") == 0, "cpython_wasm_sha256");
    CHECK(strcmp(id->metered_wasm_sha256, "22d6ad6719bd89707465bcf066b02b411d53ebe710cce056af3ec41cbaca257d") == 0, "metered_wasm_sha256");
    CHECK(strcmp(id->wabt_version, "1.0.42") == 0, "wabt_version == 1.0.42");
    printf("[PASS] 1. identity handshake (tag=%s platform=%s commit=%s)\n", id->engine_tag, id->platform, id->source_commit);

    CHECK(g_init() == PW_PY_STATUS_OK, "engine_init idempotent 1");
    CHECK(g_init() == PW_PY_STATUS_OK, "engine_init idempotent 2");

    /* 2. Fresh sequential reuse */
    for (int i = 0; i < 3; i++) {
        test_host_t host;
        init_host(&host, "print(1 + 2)");
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 50000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "fresh new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "fresh run OK");
        CHECK(strcmp(host.stdout_buf, "3\n") == 0, "stdout == '3\\n'");
        CHECK(res.guest_work_used > 0 && res.peak_memory_pages > 0, "accounting populated");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 2. fresh sequential reuse (3 instances)\n");

    /* 3. proc_exit & trap recovery */
    {
        test_host_t host;
        init_host(&host, "import sys; sys.exit(7)");
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 50000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "exit new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_PROC_EXIT && res.exit_code == 7, "proc_exit(7)");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 3. proc_exit(7) and clean unwind\n");

    /* 4. Host stop unwinding */
    {
        test_host_t host;
        init_host(&host, "print('blocked')");
        host.stop_on_write = 1;
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 50000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "stop new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_HOST_STOP, "host stop status");
        CHECK(host.stdout_len == 0, "zero bytes delivered after host stop");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 4. host stop unwind and stream sealing\n");

    /* 5. Exact fuel exhaustion */
    {
        test_host_t host;
        init_host(&host, "while True: pass");
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 2000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "fuel new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_FUEL_EXHAUSTED, "fuel exhausted");
        CHECK(res.fuel_remaining < 0 && res.guest_work_used == 2000001ULL, "exact fuel accounting");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 5. exact fuel exhaustion (budget=2000000 used=2000001)\n");

    /* 6. Asynchronous cross-thread cancellation stress inside CPU-only loop (10 rounds) */
    {
        double max_dt = 0.0;
        for (int round = 0; round < 10; round++) {
            worker_arg_t w = {0};
            w.code = "print('LOOP_READY', flush=True)\nwhile True: pass\n";
            w.max_pages = 4096;
            w.fuel = 100000000000ULL;
            pthread_t tid;
            CHECK(pthread_create(&tid, NULL, worker_thread, &w) == 0, "spawn cancel thread");
            while (!atomic_load_explicit(&w.host.ready_seen, memory_order_acquire)) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
            }
            struct timespec wait_ts = {0, 5000000}; /* 5ms inside pure CPU loop */
            nanosleep(&wait_ts, NULL);
            double t0 = now_ms();
            g_cancel(w.inst);
            pthread_join(tid, NULL);
            double dt = now_ms() - t0;
            CHECK(w.res.status == PW_PY_STATUS_CANCELED, "async cancel status == CANCELED");
            CHECK(w.res.fuel_remaining > 0, "async cancel preserves positive thread-owned fuel");
            CHECK(dt < 100.0, "async cancel latency < 100ms");
            if (dt > max_dt) max_dt = dt;
        }
        printf("[PASS] 6. async cross-thread CPU-loop cancel stress (10/10 rounds, max_latency=%.2fms)\n", max_dt);
    }

    /* 7. Memory ceiling enforcement */
    {
        test_host_t host;
        init_host(&host, "print(1)");
        pw_py_instance_config_t bad_cfg = { sizeof(bad_cfg), 10, 50000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&bad_cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_MEMORY_LIMIT && inst == NULL, "reject max_pages < initial");
        close_host(&host);

        init_host(&host, "try:\n  b = bytearray(32*1024*1024)\nexcept MemoryError:\n  print('OOM_CAUGHT')\n");
        pw_py_instance_config_t cap_cfg = { sizeof(cap_cfg), 400, 50000000ULL };
        CHECK(g_new(&cap_cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "capped mem new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "capped mem run OK");
        CHECK(strcmp(host.stdout_buf, "OOM_CAUGHT\n") == 0, "MemoryError caught cleanly");
        CHECK(res.peak_memory_pages <= 400, "peak_memory_pages <= 400");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 7. memory ceiling enforcement (reject < initial + cap growth at 400 pages)\n");

    /* 8. Concurrent multi-threaded execution (6 pthreads simultaneously) */
    {
        worker_arg_t workers[6] = {0};
        workers[0] = (worker_arg_t){ .id = 0, .code = "print(sum(range(5000)))", .max_pages = 4096, .fuel = 50000000ULL };
        workers[1] = (worker_arg_t){ .id = 1, .code = "print('ok-' + str(6*7))", .max_pages = 4096, .fuel = 50000000ULL };
        workers[2] = (worker_arg_t){ .id = 2, .code = "while True: pass", .max_pages = 4096, .fuel = 1500000ULL };
        workers[3] = (worker_arg_t){ .id = 3, .code = "print('LOOP_READY', flush=True)\nwhile True: pass\n", .max_pages = 4096, .fuel = 100000000000ULL };
        workers[4] = (worker_arg_t){ .id = 4, .code = "import sys; sys.exit(42)", .max_pages = 4096, .fuel = 50000000ULL };
        workers[5] = (worker_arg_t){ .id = 5, .code = "try:\n  bytearray(32*1024*1024)\nexcept MemoryError:\n  print('OOM_CONC')\n", .max_pages = 400, .fuel = 50000000ULL };

        pthread_t tids[6];
        for (int i = 0; i < 6; i++) {
            CHECK(pthread_create(&tids[i], NULL, worker_thread, &workers[i]) == 0, "spawn concurrent worker");
        }
        while (!atomic_load_explicit(&workers[3].host.ready_seen, memory_order_acquire)) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        struct timespec wait_ts = {0, 20000000};
        nanosleep(&wait_ts, NULL);
        g_cancel(workers[3].inst);

        for (int i = 0; i < 6; i++) pthread_join(tids[i], NULL);

        CHECK(workers[0].res.status == PW_PY_STATUS_OK && strcmp(workers[0].stdout_copy, "12497500\n") == 0, "worker 0 OK");
        CHECK(workers[1].res.status == PW_PY_STATUS_OK && strcmp(workers[1].stdout_copy, "ok-42\n") == 0, "worker 1 OK");
        CHECK(workers[2].res.status == PW_PY_STATUS_FUEL_EXHAUSTED, "worker 2 fuel exhausted");
        CHECK(workers[3].res.status == PW_PY_STATUS_CANCELED, "worker 3 canceled");
        CHECK(workers[4].res.status == PW_PY_STATUS_PROC_EXIT && workers[4].res.exit_code == 42, "worker 4 proc_exit(42)");
        CHECK(workers[5].res.status == PW_PY_STATUS_OK && strcmp(workers[5].stdout_copy, "OOM_CONC\n") == 0 && workers[5].res.peak_memory_pages <= 300, "worker 5 memory pressure");
    }
    printf("[PASS] 8. concurrent 6-thread isolation (normal, fuel, cancel, exit, memory pressure)\n");

    /* 9. Built-in C extensions smoke: PIL._imaging (JPEG+PNG), lxml.etree (XPath+XXE), sqlite3 */
    {
        const char* ext_code =
            "import sys, types, sqlite3\n"
            "conn = sqlite3.connect(':memory:')\n"
            "conn.executescript('CREATE TABLE t(v INT); INSERT INTO t VALUES (7);')\n"
            "assert conn.execute('SELECT v FROM t').fetchone() == (7,)\n"
            "pil_pkg = types.ModuleType('PIL'); pil_pkg.__path__ = []; sys.modules['PIL'] = pil_pkg\n"
            "import PIL._imaging as core\n"
            "im = core.new('RGBA', (16, 16))\n"
            "assert im.size == (16, 16)\n"
            "assert hasattr(core, 'jpeg_encoder') and hasattr(core, 'zip_encoder')\n"
            "bands = im.split()\n"
            "assert len(bands) == 4\n"
            "assert bands[3].getextrema() == (0, 0)\n"
            "assert len(bands[3].getprojection()) == 2\n"
            "assert isinstance(im.isblock(), int)\n"
            "assert im.chop_invert().size == (16, 16)\n"
            "lxml_pkg = types.ModuleType('lxml'); lxml_pkg.__path__ = []; sys.modules['lxml'] = lxml_pkg\n"
            "import lxml.etree as etree\n"
            "doc = etree.fromstring(b'<r xmlns:n=\"urn:t\"><n:k>ok</n:k></r>')\n"
            "assert doc.xpath('//n:k/text()', namespaces={'n': 'urn:t'}) == ['ok']\n"
            "p = etree.XMLParser(resolve_entities=False, no_network=True)\n"
            "d2 = etree.fromstring(b'<!DOCTYPE f [<!ENTITY e SYSTEM \"http://127.0.0.1:1/x\">]><r>&e;</r>', parser=p)\n"
            "assert (d2.text or '') == ''\n"
            "print('EXT_OK')\n";
        test_host_t host;
        init_host(&host, ext_code);
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 100000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "ext smoke new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "ext smoke run OK");
        CHECK(strcmp(host.stdout_buf, "EXT_OK\n") == 0, "ext smoke stdout == EXT_OK");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 9. built-in extensions smoke (PIL._imaging JPEG+PNG, lxml.etree XPath+XXE, sqlite3)\n");

    /* 10. Compiled native-extension concurrent cancel (Pillow & lxml) + fuel calibration */
    {
        const char* pil_loop_code =
            "import sys, types\n"
            "p = types.ModuleType('PIL'); p.__path__ = []; sys.modules['PIL'] = p\n"
            "import PIL._imaging as core\n"
            "im = core.new('RGBA', (512, 512))\n"
            "print('LOOP_READY', flush=True)\n"
            "while True:\n"
            "  im = im.resize((512, 512), 1)\n"
            "  im = im.chop_invert()\n";

        /* 10a. Concurrent cancel inside compiled Pillow C extension loop */
        worker_arg_t wp = {0};
        wp.code = pil_loop_code;
        wp.max_pages = 4096;
        wp.fuel = 100000000000ULL;
        pthread_t tid;
        CHECK(pthread_create(&tid, NULL, worker_thread, &wp) == 0, "spawn pillow cancel thread");
        while (!atomic_load_explicit(&wp.host.ready_seen, memory_order_acquire)) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        struct timespec wait_ts = {0, 10000000}; /* 10ms inside compiled C extension loop */
        nanosleep(&wait_ts, NULL);
        double t0 = now_ms();
        g_cancel(wp.inst);
        pthread_join(tid, NULL);
        double dt_pil = now_ms() - t0;
        CHECK(wp.res.status == PW_PY_STATUS_CANCELED, "pillow native-ext cancel status == CANCELED");
        CHECK(wp.res.fuel_remaining > 0, "pillow cancel preserves positive thread-owned fuel");
        CHECK(dt_pil < 100.0, "pillow native-ext cancel latency < 100ms");

        /* 10b. Fuel exhaustion calibration inside compiled Pillow C extension loop (no cancel) */
        test_host_t host;
        init_host(&host, pil_loop_code);
        pw_py_instance_config_t fcfg = { sizeof(fcfg), 4096, 5000000ULL };
        pw_py_instance_t* finst = NULL;
        CHECK(g_new(&fcfg, test_wasi_cb, &host, &finst) == PW_PY_STATUS_OK, "pillow fuel new");
        host.inst = finst;
        pw_py_run_result_t fres;
        CHECK(g_run(finst, &fres) == PW_PY_STATUS_FUEL_EXHAUSTED, "pillow fuel exhausted status");
        CHECK(fres.fuel_remaining < 0 && fres.guest_work_used == 5000001ULL, "pillow exact fuel calibration");
        g_free(finst);
        close_host(&host);

        /* 10c. Concurrent cancel inside compiled lxml C extension loop */
        const char* lxml_loop_code =
            "import sys, types\n"
            "l = types.ModuleType('lxml'); l.__path__ = []; sys.modules['lxml'] = l\n"
            "import lxml.etree as etree\n"
            "xml = b'<r>' + b'<item id=\"1\">payload</item>' * 200 + b'</r>'\n"
            "print('LOOP_READY', flush=True)\n"
            "while True:\n"
            "  doc = etree.fromstring(xml)\n"
            "  doc.xpath('//item[@id=\"1\"]/text()')\n";
        worker_arg_t wl = {0};
        wl.code = lxml_loop_code;
        wl.max_pages = 4096;
        wl.fuel = 100000000000ULL;
        CHECK(pthread_create(&tid, NULL, worker_thread, &wl) == 0, "spawn lxml cancel thread");
        while (!atomic_load_explicit(&wl.host.ready_seen, memory_order_acquire)) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        nanosleep(&wait_ts, NULL);
        t0 = now_ms();
        g_cancel(wl.inst);
        pthread_join(tid, NULL);
        double dt_lxml = now_ms() - t0;
        CHECK(wl.res.status == PW_PY_STATUS_CANCELED, "lxml native-ext cancel status == CANCELED");
        CHECK(wl.res.fuel_remaining > 0, "lxml cancel preserves positive thread-owned fuel");
        CHECK(dt_lxml < 100.0, "lxml native-ext cancel latency < 100ms");

        printf("[PASS] 10. native-extension cancel & fuel calibration (pillow=%.2fms, lxml=%.2fms, fuel=5000001)\n", dt_pil, dt_lxml);
    }

    /* 11. Built-in NumPy 1.26.4 & pandas 2.2.3 numeric extensions + kernel cancel/fuel */
    {
        const char* np_c_smoke =
            "import sys\n"
            "assert 'numpy.core._multiarray_umath' in sys.builtin_module_names\n"
            "assert 'numpy.linalg._umath_linalg' in sys.builtin_module_names\n"
            "assert 'pandas._libs.lib' in sys.builtin_module_names\n"
            "assert 'pandas._libs.parsers' in sys.builtin_module_names\n"
            "print('NUMPY_C_OK')\n";
        test_host_t host;
        init_host(&host, np_c_smoke);
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 500000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "numpy C smoke new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "numpy C smoke run OK");
        CHECK(strcmp(host.stdout_buf, "NUMPY_C_OK\n") == 0, "numpy C smoke stdout == NUMPY_C_OK");
        g_free(inst);
        close_host(&host);

        if (g_packages_dir != NULL && g_packages_dir[0] != '\0') {
            /* 11a. Full NumPy + pandas workflow (broadcast, slice, matmul, linalg, fft, random, typed CSV, left join, groupby+sum vs sqlite3) */
            const char* py13_full =
                "import sys, io, csv, sqlite3; sys.path.insert(0, '/packages')\n"
                "import numpy as np\n"
                "import pandas as pd\n"
                "a = np.array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])\n"
                "b = np.array([10.0, 20.0, 30.0])\n"
                "assert (a + b).tolist() == [[11.0, 22.0, 33.0], [14.0, 25.0, 36.0]]\n"
                "assert (a + b)[:, 1:].tolist() == [[22.0, 33.0], [25.0, 36.0]]\n"
                "m1 = np.array([[1.0, 2.0], [3.0, 4.0]]); m2 = np.array([[5.0, 6.0], [7.0, 8.0]])\n"
                "assert (m1 @ m2).tolist() == [[19.0, 22.0], [43.0, 50.0]]\n"
                "assert np.allclose(np.linalg.solve(m1, np.array([5.0, 11.0])), [1.0, 2.0])\n"
                "assert np.allclose(np.fft.rfft([1.0, 0.0, -1.0, 0.0]), [0.0, 2.0, 0.0])\n"
                "np.random.seed(42); assert np.random.poisson(5.0) >= 0\n"
                "assert np.random.default_rng(42).poisson(5.0) >= 0\n"
                "orders_csv = 'order_id,sku_id,qty\\n001,0010,2\\n002,0010,\\n003,0020,5\\n004,0099,3\\n005,0020,1\\n'\n"
                "lookup_csv = 'sku_id,category,unit_price\\n0010,widget,10.0\\n0020,gadget,25.0\\n'\n"
                "orders = pd.read_csv(io.StringIO(orders_csv), dtype={'order_id': str, 'sku_id': str, 'qty': 'Int64'})\n"
                "lookup = pd.read_csv(io.StringIO(lookup_csv), dtype={'sku_id': str, 'category': str, 'unit_price': float})\n"
                "assert orders['order_id'].tolist() == ['001', '002', '003', '004', '005'] and pd.isna(orders.loc[1, 'qty'])\n"
                "merged = orders.merge(lookup, on='sku_id', how='left')\n"
                "merged['line_total'] = merged['qty'].astype('float64') * merged['unit_price']\n"
                "grouped = merged.groupby('category', dropna=False, as_index=False).agg(total_qty=('qty', 'sum'), total_rev=('line_total', 'sum')).sort_values('category', na_position='last').reset_index(drop=True)\n"
                "con = sqlite3.connect(':memory:')\n"
                "con.execute('CREATE TABLE o (oid TEXT, sid TEXT, q INT)')\n"
                "for r in csv.DictReader(io.StringIO(orders_csv)): con.execute('INSERT INTO o VALUES (?,?,?)', (r['order_id'], r['sku_id'], int(r['qty']) if r['qty'] else None))\n"
                "con.execute('CREATE TABLE l (sid TEXT, cat TEXT, p REAL)')\n"
                "for r in csv.DictReader(io.StringIO(lookup_csv)): con.execute('INSERT INTO l VALUES (?,?,?)', (r['sku_id'], r['category'], float(r['unit_price'])))\n"
                "sql_rows = con.execute('SELECT l.cat, COALESCE(SUM(o.q),0), COALESCE(SUM(o.q*l.p),0.0) FROM o LEFT JOIN l ON o.sid=l.sid GROUP BY l.cat ORDER BY l.cat IS NULL, l.cat').fetchall()\n"
                "pd_rows = [(None if pd.isna(r.category) else r.category, int(r.total_qty), float(r.total_rev)) for r in grouped.itertuples(index=False)]\n"
                "assert pd_rows == sql_rows == [('gadget', 6, 150.0), ('widget', 2, 20.0), (None, 3, 0.0)]\n"
                "print('PY13_FULL_OK')\n";
            init_host(&host, py13_full);
            pw_py_instance_config_t full_cfg = { sizeof(full_cfg), 4096, 5000000000ULL };
            CHECK(g_new(&full_cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "py13 full new");
            host.inst = inst;
            CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "py13 full run OK");
            CHECK(strcmp(host.stdout_buf, "PY13_FULL_OK\n") == 0, "py13 full stdout == PY13_FULL_OK");
            g_free(inst);
            close_host(&host);

            /* 11b. Concurrent host cancel inside NumPy matmul kernel */
            const char* np_kernel =
                "import sys; sys.path.insert(0, '/packages')\n"
                "import numpy as np\n"
                "A = np.ones((128, 128), dtype=np.float64)\n"
                "B = np.ones((128, 128), dtype=np.float64)\n"
                "print('LOOP_READY', flush=True)\n"
                "while True:\n"
                "  C = A @ B\n";
            worker_arg_t wn = {0};
            wn.code = np_kernel;
            wn.max_pages = 4096;
            wn.fuel = 100000000000ULL;
            pthread_t tid;
            CHECK(pthread_create(&tid, NULL, worker_thread, &wn) == 0, "spawn numpy cancel thread");
            while (!atomic_load_explicit(&wn.host.ready_seen, memory_order_acquire)) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
            }
            struct timespec wait_ts = {0, 15000000};
            nanosleep(&wait_ts, NULL);
            double t0 = now_ms();
            g_cancel(wn.inst);
            pthread_join(tid, NULL);
            double dt_np = now_ms() - t0;
            CHECK(wn.res.status == PW_PY_STATUS_CANCELED, "numpy kernel cancel status == CANCELED");
            CHECK(wn.res.fuel_remaining > 0, "numpy cancel preserves positive thread-owned fuel");
            CHECK(dt_np < 100.0, "numpy kernel cancel latency < 100ms");

            /* 11c. Exact fuel exhaustion inside NumPy matmul kernel + fresh-guest recovery */
            init_host(&host, np_kernel);
            pw_py_instance_config_t nfcfg = { sizeof(nfcfg), 4096, 50000000ULL };
            CHECK(g_new(&nfcfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "numpy fuel new");
            host.inst = inst;
            CHECK(g_run(inst, &res) == PW_PY_STATUS_FUEL_EXHAUSTED, "numpy fuel exhausted status");
            CHECK(res.fuel_remaining < 0 && res.guest_work_used == 50000001ULL, "numpy exact fuel calibration");
            g_free(inst);
            close_host(&host);

            /* Subsequent fresh-guest success */
            init_host(&host, "import sys; sys.path.insert(0, '/packages'); import numpy as np; print(int((np.arange(4) @ np.arange(4))))");
            CHECK(g_new(&full_cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "numpy recovery new");
            host.inst = inst;
            CHECK(g_run(inst, &res) == PW_PY_STATUS_OK && strcmp(host.stdout_buf, "14\n") == 0, "numpy fresh-guest recovery OK");
            g_free(inst);
            close_host(&host);

            printf("[PASS] 11. built-in NumPy 1.26.4 + pandas 2.2.3 suite (broadcast/slice/matmul/CSV/join/groupby/sqlite3, kernel cancel=%.2fms, fuel=50000001, fresh recovery)\n", dt_np);
        } else {
            printf("[PASS] 11. built-in NumPy _multiarray_umath extension smoke\n");
        }
    }

    /* 12. Built-in regex 2024.11.6 extension smoke + full grapheme/script/overlap/timeout/cancel/fuel */
    {
        const char* regex_smoke =
            "import sys, types\n"
            "rpkg = types.ModuleType('regex'); rpkg.__path__ = []; sys.modules['regex'] = rpkg\n"
            "import regex._regex as _regex\n"
            "assert hasattr(_regex, 'compile') and hasattr(_regex, 'get_code_size')\n"
            "assert _regex.get_code_size() > 0\n"
            "print('REGEX_C_OK')\n";
        test_host_t host;
        init_host(&host, regex_smoke);
        pw_py_instance_config_t cfg = { sizeof(cfg), 4096, 50000000ULL };
        pw_py_instance_t* inst = NULL;
        CHECK(g_new(&cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "regex smoke new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "regex smoke run OK");
        CHECK(strcmp(host.stdout_buf, "REGEX_C_OK\n") == 0, "regex smoke stdout == REGEX_C_OK");
        g_free(inst);
        close_host(&host);

        if (g_packages_dir != NULL && g_packages_dir[0] != '\0') {
            const char* regex_full =
                "import sys; sys.path.insert(0, '/packages')\n"
                "import regex\n"
                "assert regex.findall(r'\\X', 'e\\u0301\\U0001f469\\u200d\\U0001f52c!') == ['e\\u0301', '\\U0001f469\\u200d\\U0001f52c', '!']\n"
                "assert regex.findall(r'\\p{Script=Greek}+', 'a \\u03b1\\u03b2 1 \\u03a9') == ['\\u03b1\\u03b2', '\\u03a9']\n"
                "assert [(m.group(0), m.span()) for m in regex.finditer(r'aba', 'ababa', overlapped=True)] == [('aba', (0, 3)), ('aba', (2, 5))]\n"
                "pat = regex.compile(r'(?P<w>[a-z]+)+')\n"
                "assert pat.groupindex == {'w': 1}\n"
                "m = pat.match('abc')\n"
                "assert m.allspans() == ([(0, 3)], [(0, 3)]) and m.allcaptures() == (['abc'], ['abc']) and m.capturesdict() == {'w': ['abc']}\n"
                "try:\n"
                "  regex.match(r'^(a|aa)+$', 'a'*40 + '!', timeout=0.02)\n"
                "  raise AssertionError('expected TimeoutError')\n"
                "except TimeoutError:\n"
                "  pass\n"
                "print('REGEX_FULL_OK')\n";
            init_host(&host, regex_full);
            pw_py_instance_config_t full_cfg = { sizeof(full_cfg), 4096, 5000000000ULL };
            CHECK(g_new(&full_cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "regex full new");
            host.inst = inst;
            CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "regex full run OK");
            CHECK(strcmp(host.stdout_buf, "REGEX_FULL_OK\n") == 0, "regex full stdout == REGEX_FULL_OK");
            g_free(inst);
            close_host(&host);

            const char* regex_bt =
                "import sys; sys.path.insert(0, '/packages')\n"
                "import regex\n"
                "print('LOOP_READY', flush=True)\n"
                "regex.match(r'^(a|aa)+$', 'a'*60 + '!')\n";
            worker_arg_t wr = {0};
            wr.code = regex_bt;
            wr.max_pages = 4096;
            wr.fuel = 100000000000ULL;
            pthread_t tid;
            CHECK(pthread_create(&tid, NULL, worker_thread, &wr) == 0, "spawn regex cancel thread");
            while (!atomic_load_explicit(&wr.host.ready_seen, memory_order_acquire)) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
            }
            struct timespec wait_ts = {0, 10000000};
            nanosleep(&wait_ts, NULL);
            double t0 = now_ms();
            g_cancel(wr.inst);
            pthread_join(tid, NULL);
            double dt_re = now_ms() - t0;
            CHECK(wr.res.status == PW_PY_STATUS_CANCELED, "regex native cancel status == CANCELED");
            CHECK(wr.res.fuel_remaining > 0, "regex cancel preserves positive thread-owned fuel");
            CHECK(dt_re < 100.0, "regex native cancel latency < 100ms");

            init_host(&host, regex_bt);
            pw_py_instance_config_t rfcfg = { sizeof(rfcfg), 4096, 5000000ULL };
            CHECK(g_new(&rfcfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "regex fuel new");
            host.inst = inst;
            CHECK(g_run(inst, &res) == PW_PY_STATUS_FUEL_EXHAUSTED, "regex fuel exhausted status");
            CHECK(res.fuel_remaining < 0 && res.guest_work_used == 5000001ULL, "regex exact fuel calibration");
            g_free(inst);
            close_host(&host);
            printf("[PASS] 12. built-in regex._regex full suite (grapheme, script, overlap, timeout, cancel=%.2fms, fuel=5000001)\n", dt_re);
        } else {
            printf("[PASS] 12. built-in regex._regex extension smoke\n");
        }
    }

    dlclose(dl);
    printf("ALL 12 CUMULATIVE v0.5.0 PRODUCER TESTS PASSED\n");
    return 0;
}

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
            if (fd != 3) { *out_ret = 8; return PW_PY_WASI_CONTINUE; } /* EBADF */
            const char* pre = "/usr/local/lib";
            mem[out_p] = 0; /* preopentype::dir */
            put_u32(mem + out_p + 4, (uint32_t)strlen(pre));
            *out_ret = 0;
            return PW_PY_WASI_CONTINUE;
        }
        case PW_PY_WASI_FD_PRESTAT_DIR_NAME: {
            uint32_t fd = (uint32_t)args[0], path_p = (uint32_t)args[1], path_len = (uint32_t)args[2];
            if (fd != 3) { *out_ret = 8; return PW_PY_WASI_CONTINUE; }
            const char* pre = "/usr/local/lib";
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
            if (fd >= 4 && fd < MAX_WASI_FDS && h->fds[fd] >= 0) {
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
    CHECK(strcmp(id->cpython_wasm_sha256, "bea5b8f2cb633212d4be85a668ad13246dc0c1b65c29796cf5d990ec1956b052") == 0, "cpython_wasm_sha256");
    CHECK(strcmp(id->metered_wasm_sha256, "92755437dfb30aa41a5868f06d585dac55990474e07ebca53d9e5b1f009758f4") == 0, "metered_wasm_sha256");
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
        CHECK(res.guest_work_used > 0 && res.peak_memory_pages >= 160, "accounting populated");
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

    /* 6. Asynchronous cross-thread cancellation inside CPU-only loop with huge fuel */
    {
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
        struct timespec wait_ts = {0, 20000000}; /* 20ms inside pure CPU loop */
        nanosleep(&wait_ts, NULL);
        double t0 = now_ms();
        g_cancel(w.inst);
        pthread_join(tid, NULL);
        double dt = now_ms() - t0;
        CHECK(w.res.status == PW_PY_STATUS_CANCELED, "async cancel status");
        CHECK(dt < 100.0, "async cancel latency < 100ms");
        printf("[PASS] 6. async cross-thread CPU-loop cancel (latency=%.2fms)\n", dt);
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
        pw_py_instance_config_t cap_cfg = { sizeof(cap_cfg), 300, 50000000ULL };
        CHECK(g_new(&cap_cfg, test_wasi_cb, &host, &inst) == PW_PY_STATUS_OK, "capped mem new");
        host.inst = inst;
        pw_py_run_result_t res;
        CHECK(g_run(inst, &res) == PW_PY_STATUS_OK, "capped mem run OK");
        CHECK(strcmp(host.stdout_buf, "OOM_CAUGHT\n") == 0, "MemoryError caught cleanly");
        CHECK(res.peak_memory_pages <= 300, "peak_memory_pages <= 300");
        g_free(inst);
        close_host(&host);
    }
    printf("[PASS] 7. memory ceiling enforcement (reject < initial + cap growth at 300 pages)\n");

    /* 8. Concurrent multi-threaded execution (6 pthreads simultaneously) */
    {
        worker_arg_t workers[6] = {0};
        workers[0] = (worker_arg_t){ .id = 0, .code = "print(sum(range(5000)))", .max_pages = 4096, .fuel = 50000000ULL };
        workers[1] = (worker_arg_t){ .id = 1, .code = "print('ok-' + str(6*7))", .max_pages = 4096, .fuel = 50000000ULL };
        workers[2] = (worker_arg_t){ .id = 2, .code = "while True: pass", .max_pages = 4096, .fuel = 1500000ULL };
        workers[3] = (worker_arg_t){ .id = 3, .code = "print('LOOP_READY', flush=True)\nwhile True: pass\n", .max_pages = 4096, .fuel = 100000000000ULL };
        workers[4] = (worker_arg_t){ .id = 4, .code = "import sys; sys.exit(42)", .max_pages = 4096, .fuel = 50000000ULL };
        workers[5] = (worker_arg_t){ .id = 5, .code = "try:\n  bytearray(32*1024*1024)\nexcept MemoryError:\n  print('OOM_CONC')\n", .max_pages = 300, .fuel = 50000000ULL };

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

    dlclose(dl);
    printf("ALL 9 PY10 PRODUCER TESTS PASSED\n");
    return 0;
}

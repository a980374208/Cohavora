#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* Task-owned diagnosis only. Never change the retval or termination mechanism. */
enum { MAX_FRAMES = 64, MAX_EVENTS = 4, PATH_BYTES = 192 };
typedef void (*exit_fn)(void *) __attribute__((noreturn));
static exit_fn real_exit;
static int trace_fd = -1;
static _Atomic unsigned events;
static _Atomic unsigned write_failures;
static __thread unsigned entered;
typedef int (*finalizing_fn)(void);
static finalizing_fn python_finalizing;
static int entry_fd = -1;
static _Atomic unsigned entry_events;
static _Atomic unsigned entry_write_failures;
static __thread unsigned logging_entry;

/* Separate from PXTRACE1: evidence exists before any stack walk or dladdr.
 * Linux x86-64 little-endian, fixed 64 bytes; no strings or exception payload.
 * kind: 1=pthread_exit; finalizing: 0=false, 1=true, 2=UNKNOWN.
 * monotonic_ns=0 means clock query failed. No global unwind interposition. */
struct __attribute__((packed)) entry_record {
    char magic[8];
    uint32_t version, kind;
    uint64_t pid, tid, sequence, caller_pc;
    uint32_t write_failures_before, python_finalizing;
    uint64_t monotonic_ns;
};
_Static_assert(sizeof(struct entry_record) == 64, "entry format drift");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "logging counters must be lock-free");

static void record_entry(uint32_t kind, void *caller) {
    int saved_errno = errno;
    if (!logging_entry++) {
        unsigned sequence = atomic_fetch_add_explicit(&entry_events, 1, memory_order_relaxed);
        if (sequence < 64) {
            struct timespec now;
            uint64_t monotonic_ns = 0;
            if (!syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now))
                monotonic_ns = (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
            struct entry_record record = {
                .magic = "NXENTRY2", .version = 2, .kind = kind,
                .pid = (uint64_t)getpid(), .tid = (uint64_t)syscall(SYS_gettid),
                .sequence = sequence, .caller_pc = (uint64_t)(uintptr_t)caller,
                .write_failures_before = atomic_load_explicit(&entry_write_failures, memory_order_relaxed),
                .python_finalizing = python_finalizing ? (python_finalizing() != 0) : 2,
                .monotonic_ns = monotonic_ns
            };
            ssize_t written;
            do {
                written = syscall(SYS_write, entry_fd, &record, sizeof(record));
            } while (written < 0 && errno == EINTR);
            /* Do not retry a partial append: concurrent records could interleave.
             * Decoder rejects truncation; counters expose write failures while alive. */
            if (written != (ssize_t)sizeof(record))
                atomic_fetch_add_explicit(&entry_write_failures, 1, memory_order_relaxed);
        }
    }
    --logging_entry;
    errno = saved_errno;
}

unsigned native_exit_observer_entry_events(void) {
    return atomic_load_explicit(&entry_events, memory_order_relaxed);
}
unsigned native_exit_observer_entry_write_failures(void) {
    return atomic_load_explicit(&entry_write_failures, memory_order_relaxed);
}

struct __attribute__((packed)) frame_record {
    uint64_t pc;
    uint64_t module_base;
    char module_path[PATH_BYTES];
};
struct __attribute__((packed)) event_record {
    char magic[8];
    uint32_t version;
    uint32_t frames;
    uint64_t pid;
    uint64_t tid;
    uint64_t sequence;
    uint64_t monotonic_ns;
    uint32_t resolver_ready;
    uint32_t capture_failed;
    struct frame_record frame[MAX_FRAMES];
};
_Static_assert(sizeof(struct event_record) == 13368, "trace format drift");

int native_exit_observer_ready(void) {
    return real_exit != NULL && trace_fd >= 0 && entry_fd >= 0;
}
unsigned native_exit_observer_write_failures(void) {
    return atomic_load_explicit(&write_failures, memory_order_relaxed);
}

static int owned_directory(const char *path) {
    const char *prefix = "/root/livekit-product-acceptance/native-exit-";
    if (!path || strncmp(path, prefix, strlen(prefix)) || strlen(path) > 240)
        return 0;
    for (const char *p = path + strlen(prefix); *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
               *p == '/' || *p == '-' || *p == '_' )) return 0;
    return strstr(path, "/pthread-exit-trace") != NULL;
}

__attribute__((constructor)) static void initialize_observer(void) {
    /* Must be checked by the task bootstrap before importing livekit. */
    real_exit = (exit_fn)dlsym(RTLD_NEXT, "pthread_exit");
    /* Optional read-only CPython scalar API; absence is UNKNOWN, not launch failure. */
    python_finalizing = (finalizing_fn)dlsym(RTLD_DEFAULT, "_Py_IsFinalizing");
    const char *directory = getenv("NATIVE_EXIT_TRACE_DIR");
    if (!real_exit || !owned_directory(directory)) return;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return;
    char name[48];
    int length = snprintf(name, sizeof(name), "pthread-exit-%ld.bin", (long)getpid());
    if (length > 0 && (size_t)length < sizeof(name))
        trace_fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_APPEND |
                          O_NOFOLLOW | O_CLOEXEC, 0600);
    length = snprintf(name, sizeof(name), "unwind-entry-%ld.bin", (long)getpid());
    if (length > 0 && (size_t)length < sizeof(name))
        entry_fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_APPEND |
                          O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    /* Preload libgcc/backtrace machinery before SDK import. This is an intervention. */
    void *warmup[8];
    if (trace_fd >= 0 && backtrace(warmup, 8) <= 0) {
        close(trace_fd);
        trace_fd = -1;
    }
}

void pthread_exit(void *retval) {
    record_entry(1, __builtin_extract_return_addr(__builtin_return_address(0)));
    /* Resolver failure is a pre-import launch failure, never a synthetic exit fallback. */
    exit_fn target = real_exit;
    if (!target) target = (exit_fn)dlsym(RTLD_NEXT, "pthread_exit");
    if (!entered++) {
        unsigned sequence = atomic_fetch_add_explicit(&events, 1, memory_order_relaxed);
        if (sequence < MAX_EVENTS) {
            struct event_record record = {0};
            memcpy(record.magic, "PXTRACE1", 8);
            record.version = 1;
            record.pid = (uint64_t)getpid();
            record.tid = (uint64_t)syscall(SYS_gettid);
            record.sequence = sequence;
            record.resolver_ready = target != NULL;
            struct timespec now;
            int clock_failed = 0;
            if (!clock_gettime(CLOCK_MONOTONIC, &now))
                record.monotonic_ns = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
            else clock_failed = 1;
            void *addresses[MAX_FRAMES];
            int count = backtrace(addresses, MAX_FRAMES);
            record.frames = count > 0 ? (uint32_t)count : 0;
            record.capture_failed = clock_failed || count <= 0 || trace_fd < 0 || target == NULL;
            for (int i = 0; i < count; ++i) {
                Dl_info info = {0};
                record.frame[i].pc = (uint64_t)(uintptr_t)addresses[i];
                if (dladdr(addresses[i], &info) && info.dli_fname) {
                    record.frame[i].module_base = (uint64_t)(uintptr_t)info.dli_fbase;
                    size_t n = strnlen(info.dli_fname, PATH_BYTES);
                    if (n < PATH_BYTES) memcpy(record.frame[i].module_path, info.dli_fname, n);
                }
            }
            const char *bytes = (const char *)&record;
            size_t left = sizeof(record);
            while (trace_fd >= 0 && left) {
                ssize_t written = write(trace_fd, bytes, left);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) break;
                bytes += written;
                left -= (size_t)written;
            }
            if (left) atomic_fetch_add_explicit(&write_failures, 1, memory_order_relaxed);
        }
    }
    /* No abort/kill/_exit, retval rewrite, pthread cancellation, or swallowed unwind. */
    target(retval);
    __builtin_unreachable();
}

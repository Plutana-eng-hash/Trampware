// Trampware file log: /data/trampware/trampware.log
// Every line is opened, appended and closed on its own, so the last line before a crash is always on disk.
// The log is rotated to trampware.old.log when it passes 4 MB.
// /data/trampware/disabled_titles.txt lists title IDs (one per line) that Trampware must stay off for.
#ifndef TW_LOG_H
#define TW_LOG_H
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TW_DIR "/data/trampware"
#define TW_LOG_FILE TW_DIR "/trampware.log"
#define TW_LOG_OLD TW_DIR "/trampware.old.log"
#define TW_LOG_MAX (4u * 1024u * 1024u)
#define TW_DISABLED_FILE TW_DIR "/disabled_titles.txt"

static volatile int g_twLock;
static u64 g_twT0;

static u64 tw_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull;
}

static void tw_lock(void) { while (__atomic_exchange_n(&g_twLock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(20); }
static void tw_unlock(void) { __atomic_store_n(&g_twLock, 0, __ATOMIC_RELEASE); }

static void tw_vlogf(const char* fmt, va_list ap) {
    tw_lock();
    FILE* f = fopen(TW_LOG_FILE, "a");
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) > (long)TW_LOG_MAX) {
            fclose(f);
            rename(TW_LOG_FILE, TW_LOG_OLD);
            f = fopen(TW_LOG_FILE, "a");
        }
    }
    if (f) {
        const u64 t = tw_ms() - g_twT0;
        fprintf(f, "[%llu.%03llu] ", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000));
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    tw_unlock();
}

static void tw_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tw_vlogf(fmt, ap);
    va_end(ap);
}

static void tw_init_dirs(void) {
    mkdir(TW_DIR, 0777);
    FILE* f = fopen(TW_DISABLED_FILE, "r");
    if (f) { fclose(f); return; }
    f = fopen(TW_DISABLED_FILE, "w");
    if (f) {
        fprintf(f, "# Trampware: put a title ID on its own line to keep Trampware off for that game.\n");
        fprintf(f, "# The title ID of the running game is written at the top of trampware.log.\n");
        fclose(f);
    }
}

static bool tw_title_disabled(const char* tid) {
    FILE* f = fopen(TW_DISABLED_FILE, "r");
    if (!f) return false;
    char line[64];
    bool hit = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        if (strcmp(line, tid) == 0) { hit = true; break; }
    }
    fclose(f);
    return hit;
}

#endif

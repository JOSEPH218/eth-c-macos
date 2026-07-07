#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "log.h"

static int g_verbose = 0;

void log_set_verbose(int verbose) { g_verbose = verbose; }

static void log_line(const char *level, const char *fmt, va_list ap) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    fprintf(stderr, "[%s] %-5s ", ts, level);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

void log_info(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    log_line("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_line("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_line("ERROR", fmt, ap);
    va_end(ap);
}

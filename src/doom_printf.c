// Minimal printf family for the CHDK DOOM blob. newlib's formatted-output (vfprintf) hangs
// in this bare-metal/uncached setup (plain strings print, but the %d/%x/%p conversion path
// does not). DOOM only formats ints/strings/pointers, so this compact formatter replaces the
// whole family; output is line-buffered to the launcher's on-screen log. File IO stays on
// newlib stdio (fopen/fread) which is a different path and works.
#include <stdarg.h>
#include <stddef.h>
#include "doom_chdk_abi.h"
extern doom_cb_t *g_cb;

// --- line-buffered console -> g_cb->log -------------------------------------
static char L[200];
static int  Ln = 0;
static void emitc(char c) {
    if (c == '\n' || Ln >= (int)sizeof(L) - 1) {
        L[Ln] = 0;
        if (g_cb && g_cb->log) g_cb->log(L);
        Ln = 0;
        if (c == '\n') return;
    }
    L[Ln++] = c;
}
static void emits(const char *s) { while (*s) emitc(*s++); }

static void utoa_base(unsigned long v, char *out, int base, int upper) {
    char tmp[34]; int i = 0;
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = d[v % base]; v /= base; }
    int j = 0; while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

// --- core formatter ---------------------------------------------------------
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    #define PUT(ch) do { if (pos + 1 < size) buf[pos] = (char)(ch); pos++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, lng = 0, neg = 0, prec = -1, is_num = 0, is_str = 0;
        while (*fmt == '-' || *fmt == '0') { if (*fmt == '-') left = 1; else zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; } }
        if (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') fmt++; }
        char tmp[36]; const char *s = tmp;
        switch (*fmt) {
            case 'd': case 'i': {
                long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
                unsigned long uv; if (v < 0) { neg = 1; uv = (unsigned long)(-v); } else uv = (unsigned long)v;
                utoa_base(uv, tmp, 10, 0); break; }
            case 'u': { unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned); utoa_base(v, tmp, 10, 0); break; }
            case 'x': { unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned); utoa_base(v, tmp, 16, 0); break; }
            case 'X': { unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned); utoa_base(v, tmp, 16, 1); break; }
            case 'p': { unsigned long v = (unsigned long)va_arg(ap, void *); tmp[0] = '0'; tmp[1] = 'x'; utoa_base(v, tmp + 2, 16, 0); break; }
            case 'c': { tmp[0] = (char)va_arg(ap, int); tmp[1] = 0; break; }
            case 's': { s = va_arg(ap, const char *); if (!s) s = "(null)"; break; }
            case '%': { tmp[0] = '%'; tmp[1] = 0; break; }
            default:  { tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; if (!*fmt) fmt--; break; }
        }
        is_num = (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X' || *fmt == 'p');
        is_str = (*fmt == 's');
        int len = 0; while (s[len]) len++;
        if (is_str && prec >= 0 && len > prec) len = prec;           // %.Ns = max chars
        int zpad = (is_num && prec >= 0 && len < prec) ? prec - len : 0;  // %.Nd = min digits (zeros)
        int wpad = width - len - neg - zpad; if (wpad < 0) wpad = 0;
        int k;
        if (!left) {
            if (zero && !is_str && prec < 0) { if (neg) { PUT('-'); neg = 0; } while (wpad-- > 0) PUT('0'); }
            else { while (wpad-- > 0) PUT(' '); }
        }
        if (neg) PUT('-');
        while (zpad-- > 0) PUT('0');
        for (k = 0; k < len; k++) PUT(s[k]);
        if (left) { while (wpad-- > 0) PUT(' '); }
    }
    if (size > 0) buf[pos < size ? pos : size - 1] = 0;
    return (int)pos;
    #undef PUT
}

int vsprintf(char *b, const char *f, va_list a) { return vsnprintf(b, 0x7fffffff, f, a); }
int snprintf(char *b, size_t n, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, n, f, a); va_end(a); return r; }
int sprintf(char *b, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, 0x7fffffff, f, a); va_end(a); return r; }
int vprintf(const char *f, va_list a) { char b[256]; int r = vsnprintf(b, sizeof(b), f, a); emits(b); return r; }
int printf(const char *f, ...) { va_list a; va_start(a, f); int r = vprintf(f, a); va_end(a); return r; }
int vfprintf(void *fp, const char *f, va_list a) { (void)fp; return vprintf(f, a); }
int fprintf(void *fp, const char *f, ...) { (void)fp; va_list a; va_start(a, f); int r = vprintf(f, a); va_end(a); return r; }
int puts(const char *s) { emits(s); emitc('\n'); return 0; }
int putchar(int c) { emitc((char)c); return c; }
int fputs(const char *s, void *fp) { (void)fp; emits(s); return 0; }
int fputc(int c, void *fp) { (void)fp; emitc((char)c); return c; }
int sscanf(const char *s, const char *f, ...) { (void)s; (void)f; return 0; }  // stub -> config uses defaults

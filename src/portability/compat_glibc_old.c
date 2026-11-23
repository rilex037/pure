/* PurePlayer portability shim: symbols referenced because the build
 * machine's glibc 2.41 headers redirect them (_GNU_SOURCE now implies
 * _ISOC2Y_SOURCE), but which do not exist on glibc < 2.38. Defined here so
 * the final binary only depends on old symbol versions.
 * Also provides strlcpy/strlcat (glibc >= 2.38 only) for SDL. */
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

long __isoc23_strtol(const char *nptr, char **endptr, int base) { return strtol(nptr, endptr, base); }
unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) { return strtoul(nptr, endptr, base); }
long long __isoc23_strtoll(const char *nptr, char **endptr, int base) { return strtoll(nptr, endptr, base); }
unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base) { return strtoull(nptr, endptr, base); }
long __isoc23_wcstol(const wchar_t *nptr, wchar_t **endptr, int base) { return wcstol(nptr, endptr, base); }
unsigned long __isoc23_wcstoul(const wchar_t *nptr, wchar_t **endptr, int base) { return wcstoul(nptr, endptr, base); }
int __isoc23_fscanf(FILE *stream, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vfscanf(stream, fmt, a); va_end(a); return r; }
int __isoc23_scanf(const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vscanf(fmt, a); va_end(a); return r; }
int __isoc23_sscanf(const char *str, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vsscanf(str, fmt, a); va_end(a); return r; }
int __isoc23_vsscanf(const char *str, const char *fmt, va_list ap) { return vsscanf(str, fmt, ap); }
int __isoc23_vfscanf(FILE *stream, const char *fmt, va_list ap) { return vfscanf(stream, fmt, ap); }

size_t strlcpy(char *dst, const char *src, size_t siz)
{
    const char *s = src;
    size_t n = siz;
    if (n != 0) {
        while (--n != 0) {
            if ((*dst++ = *s++) == '\0')
                break;
        }
    }
    if (n == 0) {
        if (siz != 0)
            *dst = '\0';
        while (*s++) { }
    }
    return (size_t)(s - src - 1);
}

size_t strlcat(char *dst, const char *src, size_t siz)
{
    char *d = dst;
    const char *s = src;
    size_t n = siz, dlen;
    while (n-- != 0 && *d != '\0')
        d++;
    dlen = (size_t)(d - dst);
    n = siz - dlen;
    if (n == 0)
        return dlen + strlen(s);
    while (*s != '\0') {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = '\0';
    return dlen + (size_t)(s - src);
}

size_t wcslcpy(wchar_t *dst, const wchar_t *src, size_t siz)
{
    const wchar_t *s = src;
    size_t n = siz;
    if (n != 0) {
        while (--n != 0) {
            if ((*dst++ = *s++) == L'\0')
                break;
        }
    }
    if (n == 0) {
        if (siz != 0)
            *dst = L'\0';
        while (*s++) { }
    }
    return (size_t)(s - src - 1);
}

size_t wcslcat(wchar_t *dst, const wchar_t *src, size_t siz)
{
    wchar_t *d = dst;
    const wchar_t *s = src;
    size_t n = siz, dlen;
    while (n-- != 0 && *d != L'\0')
        d++;
    dlen = (size_t)(d - dst);
    n = siz - dlen;
    if (n == 0)
        return dlen + wcslen(s);
    while (*s != L'\0') {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = L'\0';
    return dlen + (size_t)(s - src);
}

/* What freestanding gcc may call from cases.c (in both the 32-bit image and the native build). */
#include <stddef.h>

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b)
        while (n--) *a++ = *b++;
    else
        while (n--) a[n] = b[n];
    return d;
}
void *memcpy(void *d, const void *s, size_t n) { return memmove(d, s, n); }

/* Math gcc may call instead of inlining (at -O0). */
double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    long long i = (long long)x;
    double d = (double)i;
    return d > x ? d - 1.0 : d;
}
double trunc(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    return (double)(long long)x;
}
double ceil(double x) { return -floor(-x); }

#include "./string.h"
#include <stdint.h>
#include <stddef.h>
#include "./stdlib.h"
#include "./ctype.h"

/**
 * K&R implementation
 */
void int_to_ascii(int n, char str[]) {
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);

    if (sign < 0) str[i++] = '-';
    str[i] = '\0';

    reverse(str);
}

void hex_to_ascii(int n, char str[]) {
    append(str, '0');
    append(str, 'x');
    char zeros = 0;

    int32_t tmp;
    int i;
    for (i = 28; i > 0; i -= 4) {
        tmp = (n >> i) & 0xF;
        if (tmp == 0 && zeros == 0) continue;
        zeros = 1;
        if (tmp > 0xA) append(str, tmp - 0xA + 'a');
        else append(str, tmp + '0');
    }

    tmp = n & 0xF;
    if (tmp >= 0xA) append(str, tmp - 0xA + 'a');
    else append(str, tmp + '0');
}

/* K&R */
void reverse(char s[]) {
    size_t n = strlen(s);
    if (n == 0) return;
    for (size_t i = 0, j = n - 1; i < j; i++, j--) {
        int c = s[i];
        s[i] = s[j];
        s[j] = (char)c;
    }
}

/* K&R */
size_t strlen(char s[]) {
    size_t i = 0;
    while (s[i] != '\0') ++i;
    return i;
}

void append(char s[], char n) {
    size_t len = strlen(s);
    s[len] = n;
    s[len + 1] = '\0';
}

void backspace(char s[]) {
    size_t len = strlen(s);
    if (len > 0) s[len - 1] = '\0';
}

/* K&R
 * Returns <0 if s1<s2, 0 if s1==s2, >0 if s1>s2 */
int strcmp(char s1[], char s2[]) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

/* ── New string functions ──────────────────────────────────────────────── */

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i;
    for (i = 0; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dst;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0')  return 0;
    }
    return 0;
}

int strcasecmp(const char *s1, const char *s2) {
    int i;
    for (i = 0; s1[i] && s2[i]; i++) {
        int c = tolower((unsigned char)s1[i]) - tolower((unsigned char)s2[i]);
        if (c) return c;
    }
    return (unsigned char)s1[i] - (unsigned char)s2[i];
}

char *strchr(const char *s, int c) {
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return (unsigned char)c == 0 ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c) {
    char *last = (char *)0;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) last = (char *)s;
    if ((unsigned char)c == 0) return (char *)s;
    return last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen((char *)needle);
    for (; *haystack; haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return (char *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; *s; s++, n++)
        if (!strchr(accept, (unsigned char)*s)) break;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; *s; s++, n++)
        if (strchr(reject, (unsigned char)*s)) break;
    return n;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t i;
    for (i = 0; i < maxlen && s[i]; i++);
    return i;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *s = str ? str : *saveptr;
    if (!s) return (char *)0;
    s += strspn(s, delim);
    if (!*s) { *saveptr = s; return (char *)0; }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) { *s++ = '\0'; }
    *saveptr = s;
    return tok;
}

/* ── ────────────────────────────────────────────────────────────────────── */

#define UINT64_MAX_VAL ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

uint64_t str_to_uint64(const char *s, uint8_t *error)
{
    *error = 1;

    if (!s) return 0;

    // Skip leading whitespace
    while (isspace(*s)) s++;

    if (!isdigit(*s)) return 0;   // empty or invalid

    uint64_t result = 0;
    while (isdigit(*s))
    {
        uint8_t digit = (uint8_t)(*s - '0');

        // Overflow check: result * 10 + digit > UINT64_MAX
        if (result > (UINT64_MAX_VAL - digit) / 10) return 0;

        result = result * 10 + digit;
        s++;
    }

    // Trailing garbage
    while (isspace(*s)) s++;
    if (*s != '\0') return 0;

    *error = 0;
    return result;
}

#define INT64_MAX_VAL ((int64_t)0x7FFFFFFFFFFFFFFFLL)
#define INT64_MIN_VAL ((int64_t)(-INT64_MAX_VAL - 1))

int64_t str_to_int64(const char *s, uint8_t *error)
{
    *error = 1;

    if (!s) return 0;

    while (isspace(*s)) s++;

    int negative = 0;
    if (*s == '-')      { negative = 1; s++; }
    else if (*s == '+') { s++; }

    if (!isdigit(*s)) return 0;

    // Parse absolute value as uint64 to handle INT64_MIN cleanly
    uint64_t result = 0;
    uint64_t limit  = negative
        ? (uint64_t)INT64_MAX_VAL + 1   // abs(INT64_MIN)
        : (uint64_t)INT64_MAX_VAL;

    while (isdigit(*s))
    {
        uint8_t digit = (uint8_t)(*s - '0');

        if (result > (limit - digit) / 10) return 0;  // overflow

        result = result * 10 + digit;
        s++;
    }

    while (isspace(*s)) s++;
    if (*s != '\0') return 0;

    *error = 0;
    return negative ? -(int64_t)result : (int64_t)result;
}

double str_to_double(const char *s, uint8_t *error)
{
    *error = 1;

    if (!s) return 0.0;

    while (isspace(*s)) s++;

    int negative = 0;
    if (*s == '-')      { negative = 1; s++; }
    else if (*s == '+') { s++; }

    // Must start with a digit or a lone '.' followed by a digit
    if (!isdigit(*s) && (*s != '.' || !isdigit(*(s + 1)))) return 0.0;

    // Integer part
    double result = 0.0;
    while (isdigit(*s))
    {
        result = result * 10.0 + (*s - '0');
        s++;
    }

    // Fractional part
    if (*s == '.')
    {
        s++;
        double factor = 0.1;
        while (isdigit(*s))
        {
            result += (*s - '0') * factor;
            factor *= 0.1;
            s++;
        }
    }

    // Exponent part  (e / E)
    if (*s == 'e' || *s == 'E')
    {
        s++;
        int exp_negative = 0;
        if (*s == '-')      { exp_negative = 1; s++; }
        else if (*s == '+') { s++; }

        if (!isdigit(*s)) return 0.0;   // 'e' with no digits

        int exp = 0;
        while (isdigit(*s))
        {
            exp = exp * 10 + (*s - '0');
            if (exp > 9999) exp = 9999;  // clamp — avoids runaway loop
            s++;
        }

        // Raise 10^exp by repeated squaring
        double base  = 10.0;
        double power = 1.0;
        while (exp > 0)
        {
            if (exp & 1) power *= base;
            base *= base;
            exp >>= 1;
        }

        result = exp_negative ? result / power : result * power;
    }

    while (isspace(*s)) s++;
    if (*s != '\0') return 0.0;

    *error = 0;
    return negative ? -result : result;
}

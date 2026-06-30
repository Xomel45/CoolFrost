#ifndef STRINGS_H
#define STRINGS_H

#include <stdint.h>
#include <stddef.h>

/* ── Original ──────────────────────────────────────────────────────────── */
void   int_to_ascii(int n, char str[]);
void   hex_to_ascii(int n, char str[]);
void   reverse(char s[]);
size_t strlen(char s[]);
void   backspace(char s[]);
void   append(char s[], char n);
int    strcmp(char s1[], char s2[]);

/* ── Copy / concatenate ─────────────────────────────────────────────────── */
char  *strcpy (char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat (char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);

/* ── Compare ────────────────────────────────────────────────────────────── */
int    strncmp(const char *s1, const char *s2, size_t n);
int    strcasecmp(const char *s1, const char *s2);

/* ── Search ─────────────────────────────────────────────────────────────── */
char  *strchr (const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr (const char *haystack, const char *needle);
size_t strspn (const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/* ── Length ─────────────────────────────────────────────────────────────── */
size_t strnlen(const char *s, size_t maxlen);

/* ── Tokenise (re-entrant) ──────────────────────────────────────────────── */
char  *strtok_r(char *str, const char *delim, char **saveptr);

/* ── Parse ──────────────────────────────────────────────────────────────── */
uint64_t str_to_uint64(const char *s, uint8_t *error);
int64_t  str_to_int64 (const char *s, uint8_t *error);
double   str_to_double(const char *s, uint8_t *error);

#endif

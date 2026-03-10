#ifndef STRINGS_H
#define STRINGS_H

#include <stdint.h>
#include <stddef.h>

void int_to_ascii(int n, char str[]);
void hex_to_ascii(int n, char str[]);
void reverse(char s[]);
size_t strlen(char s[]);
void backspace(char s[]);
void append(char s[], char n);
int strcmp(char s1[], char s2[]);
uint64_t str_to_uint64(const char *s, uint8_t *error);
int64_t str_to_int64(const char *s, uint8_t *error);
double str_to_double(const char *s, uint8_t *error);

#endif

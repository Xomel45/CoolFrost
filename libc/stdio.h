/*
FrostOS implementation of stdio.h
- Made by Ilonic 2025
*/
#ifndef STDIO_H
#define STDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define NULL ((void*)0)
#define EOF -1

int    sprintf (char *s, const char *format, ...);
void   printf  (const char *format, ...);
int    snprintf(char *s, size_t n, const char *format, ...);
int    vsnprintf(char *s, size_t n, const char *format, va_list args);

#endif

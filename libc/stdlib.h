/*
FrostOS implementation of stdlib.h
- Made by Ilonic 2025
*/
#ifndef STDLIB_H
#define STDLIB_H

#include <stdint.h>
#include <stddef.h>
#include "./mem.h"
#include "./string.h"

char *itoa(int64_t value, char * str, int base);
double pow (double x, int y);
void llimit(char *src, size_t n, char a);
void rlimit(char *src, size_t n, char a);
char *ftoa(double value, char *str, int base, int precision);

#endif
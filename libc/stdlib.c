#include "./stdlib.h"
#include <stdint.h>

// // Took from OSDEV website
char *itoa(int64_t value, char * str, int base)
{
    char *rc;
    char *ptr;
    char *low;
    // Check for supported base.
    if ( base < 2 || base > 36 )
    {
        *str = '\0';
        return str;
    }
    rc = ptr = str;
    // Set '-' for negative decimals.
    if ( value < 0 && base == 10 )
    {
        *ptr++ = '-';
    }
    // Remember where the numbers start.
    low = ptr;
    // The actual conversion.
    do
    {
        // Modulo is negative for negative value. This trick makes abs() unnecessary.
        *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35 + value % base];
        value /= base;
    } while ( value );
    // Terminating the string.
    *ptr-- = '\0';
    // Invert the numbers.
    while ( low < ptr )
    {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return rc;
}

double pow(double x, int y)
{
    double temp;
    if (y == 0)
        return 1;
    temp = pow(x, y / 2);
    if ((y % 2) == 0) {
        return temp * temp;
    } else {
        if (y > 0)
            return x * temp * temp;
        else
            return (temp * temp) / x;
    }
}

void llimit(char *src, size_t n, char a) {
    size_t len = strlen(src);
    if (len == n) return;
    if (len > n) {
        src[n] = '\0';
        return;
    }
    for (size_t i = len; i < n; i++)
        src[i] = a;
    src[n] = '\0';
}

void rlimit(char *src, size_t n, char a) {
    size_t len = strlen(src);
    if (len >= n-1) {
        src[n-1] = '\0';
        return;
    }

    size_t pad = (n - 1) - len;

    memmove(src + pad, src, len + 1);
    for (size_t i = 0; i < pad; i++)
        src[i] = a;
}

// FIXME: When precision >= 10 weird bugs appear
char *ftoa(double value, char *str, int base, int precision)
{
    char *p = str;

    // Обработка знака
    if (value < 0) {
        *p++ = '-';
        value = -value;
    }

    // Целая часть
    int int_part = (int)value;
    double frac_part = value - (double)int_part;

    // Преобразуем целую часть
    char int_buf[32];
    itoa(int_part, int_buf, base);

    // Копируем целую часть
    char *t = int_buf;
    while (*t) *p++ = *t++;

    // Если precision = 0 → вернуть только целую часть
    if (precision <= 0) {
        *p = '\0';
        return str;
    }

    // Точка
    *p++ = '.';

    // Дробная часть
    double power = pow((double)base, precision);
    // используем unsigned int чтобы избежать проблем с отрицательными числами
    unsigned long frac_int = (unsigned long)(frac_part * power + 0.5);

    // Преобразуем дробную часть
    char frac_buf[64];
    itoa(frac_int, frac_buf, base);

    // Добавляем ведущие нули
    llimit(frac_buf, precision, '0');

    // Копируем дробную часть
    t = frac_buf;
    while (*t) *p++ = *t++;

    *p = '\0';
    return str;
}

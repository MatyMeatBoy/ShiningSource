/* The small C library of the game image. File, environment and math calls go to the host through the bridge;
 * memory and string routines and the formatter are implemented here. */
#include "bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ss_file { int handle; };
static struct ss_file standard_error = { 2 }, standard_output = { 1 }, files[16];
FILE *const stderr = &standard_error;
FILE *const stdout = &standard_output;

void *memcpy(void *destination, const void *source, size_t size)
{
    unsigned char *d = destination;
    const unsigned char *s = source;
    while (size--) *d++ = *s++;
    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *d = destination;
    const unsigned char *s = source;
    if (d < s) {
        while (size--) *d++ = *s++;
    } else {
        while (size--) d[size] = s[size];
    }
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    unsigned char *d = destination;
    while (size--) *d++ = (unsigned char)value;
    return destination;
}

int memcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *x = a, *y = b;
    for (; size--; x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *text)
{
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

unsigned long strtoul(const char *text, char **end, int base)
{
    unsigned long value = 0;
    while (*text == ' ') text++;
    for (;; text++) {
        int digit = *text >= '0' && *text <= '9' ? *text - '0' : *text >= 'a' && *text <= 'f' ? *text - 'a' + 10
                    : *text >= 'A' && *text <= 'F' ? *text - 'A' + 10 : 99;
        if (digit >= base) break;
        value = value * (unsigned long)base + (unsigned long)digit;
    }
    if (end) *end = (char *)text;
    return value;
}

/* Formatter: %c %s %d %i %u %x %X %p with the l modifier, zero padding and a width. */
int vsnprintf(char *out, size_t size, const char *format, va_list arguments)
{
    size_t length = 0;
#define PUT(ch) do { if (length + 1 < size) out[length] = (char)(ch); length++; } while (0)
    for (; *format; format++) {
        char digits[24], pad = ' ';
        const char *text;
        int width = 0, count = 0, is_long = 0, negative = 0, base = 10, upper = 0;
        unsigned long value;

        if (*format != '%') { PUT(*format); continue; }
        format++;
        if (*format == '0') { pad = '0'; format++; }
        while (*format >= '0' && *format <= '9') width = width * 10 + (*format++ - '0');
        while (*format == 'l') { is_long = 1; format++; }
        switch (*format) {
        case '%': PUT('%'); continue;
        case 'c': PUT(va_arg(arguments, int)); continue;
        case 's':
            text = va_arg(arguments, const char *);
            if (!text) text = "(null)";
            for (count = (int)strlen(text); count < width; count++) PUT(' ');
            while (*text) PUT(*text++);
            continue;
        case 'd': case 'i': {
            long signed_value = is_long ? va_arg(arguments, long) : va_arg(arguments, int);
            negative = signed_value < 0;
            value = negative ? 0ul - (unsigned long)signed_value : (unsigned long)signed_value;
            break;
        }
        case 'p': base = 16; upper = 1; value = (unsigned long)(uintptr_t)va_arg(arguments, void *); pad = '0'; width = 8; break;
        case 'X': upper = 1; /* fall through */
        case 'x': base = 16; /* fall through */
        case 'u': value = is_long ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned int); break;
        default: PUT('?'); continue;
        }
        do {
            int digit = (int)(value % (unsigned long)base);
            digits[count++] = (char)(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
            value /= (unsigned long)base;
        } while (value);
        if (negative && pad == '0') PUT('-');
        for (width -= count + negative; width > 0; width--) PUT(pad);
        if (negative && pad != '0') PUT('-');
        while (count) PUT(digits[--count]);
    }
#undef PUT
    if (size) out[length < size ? length : size - 1] = 0;
    return (int)length;
}

int snprintf(char *out, size_t size, const char *format, ...)
{
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return length;
}

int fprintf(FILE *file, const char *format, ...)
{
    char text[1024];
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (length > (int)sizeof text - 1) length = (int)sizeof text - 1;
    SS_HOST(SS_B_WRITE, int (*)(int, uint32_t, uint32_t))(file->handle, (uint32_t)(uintptr_t)text, (uint32_t)length);
    return length;
}

FILE *fopen(const char *path, const char *mode)
{
    int handle = SS_HOST(SS_B_OPEN, int (*)(uint32_t, uint32_t))((uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)mode), i;
    if (!handle)
        return NULL;
    for (i = 0; i < 16; i++) {
        if (!files[i].handle) {
            files[i].handle = handle;
            return &files[i];
        }
    }
    SS_HOST(SS_B_CLOSE, int (*)(int))(handle);
    return NULL;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *file)
{
    int got = SS_HOST(SS_B_READ, int (*)(int, uint32_t, uint32_t))(file->handle, (uint32_t)(uintptr_t)buffer, (uint32_t)(size * count));
    return size ? (size_t)(got < 0 ? 0 : got) / size : 0;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *file)
{
    int put = SS_HOST(SS_B_WRITE, int (*)(int, uint32_t, uint32_t))(file->handle, (uint32_t)(uintptr_t)buffer, (uint32_t)(size * count));
    return size ? (size_t)(put < 0 ? 0 : put) / size : 0;
}

int fclose(FILE *file)
{
    int result = SS_HOST(SS_B_CLOSE, int (*)(int))(file->handle);
    file->handle = 0;
    return result;
}

int fflush(FILE *file) { (void)file; return 0; }
int remove(const char *path) { return SS_HOST(SS_B_REMOVE, int (*)(uint32_t))((uint32_t)(uintptr_t)path); }
int rename(const char *from, const char *to) { return SS_HOST(SS_B_RENAME, int (*)(uint32_t, uint32_t))((uint32_t)(uintptr_t)from, (uint32_t)(uintptr_t)to); }

char *getenv(const char *name)
{
    /* A value stays valid for the life of the program, as callers of getenv expect: each name gets its own
     * place in the pool the first time it is asked for. Test scripts in SS_INPUT can be tens of kilobytes. */
    static char pool[0x60000], names[64][24];
    static char *values[64];
    static int count, used;
    size_t size = strlen(name) + 1;
    int i, length;

    for (i = 0; i < count; i++)
        if (size <= sizeof names[i] && !memcmp(names[i], name, size))
            return values[i];
    if (used + 0x20000 > (int)sizeof pool)
        used = sizeof pool - 0x20000;           /* pool exhausted: the last place is reused */
    length = SS_HOST(SS_B_GETENV, int (*)(uint32_t, uint32_t, uint32_t))((uint32_t)(uintptr_t)name, (uint32_t)(uintptr_t)(pool + used), 0x20000);
    if (count < 64 && size <= sizeof names[0]) {
        memcpy(names[count], name, size);
        values[count++] = length < 0 ? NULL : pool + used;
    }
    if (length < 0)
        return NULL;
    used += length + 1;
    return pool + used - length - 1;
}

long strtol(const char *text, char **end, int base)
{
    while (*text == ' ')
        text++;
    if (*text == '-')
        return -(long)strtoul(text + 1, end, base);
    return (long)strtoul(*text == '+' ? text + 1 : text, end, base);
}

/* Functions to run when the program ends (a few: test reports). */
static void (*at_exit[8])(void);
static int at_exit_count;

int atexit(void (*function)(void))
{
    if (at_exit_count == 8)
        return -1;
    at_exit[at_exit_count++] = function;
    return 0;
}

double floor(double x)
{
    long long whole = (long long)x;         /* towards zero; the values here are far inside the range */
    return (double)(whole - (x < (double)whole));
}

void exit(int code)
{
    while (at_exit_count > 0)
        at_exit[--at_exit_count]();
    SS_HOST(SS_B_EXIT, void (*)(int))(code);
    for (;;) {}
}

double pow(double base, double exponent) { return SS_HOST(SS_B_POW, double (*)(double, double))(base, exponent); }
double sin(double x) { return SS_HOST(SS_B_SIN, double (*)(double))(x); }
double cos(double x) { return SS_HOST(SS_B_COS, double (*)(double))(x); }
double fmod(double x, double y) { return SS_HOST(SS_B_FMOD, double (*)(double, double))(x, y); }
long lround(double x) { return (long)(x < 0 ? x - 0.5 : x + 0.5); }

/* clang for the arm64_32 target turns a zeroing memset into this call */
void bzero(void *destination, size_t size)
{
    memset(destination, 0, size);
}

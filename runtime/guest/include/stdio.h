/* Minimal C library of the game image: only what the runtime core uses. Implemented in ../libc.c. */
#ifndef SS_GUEST_STDIO_H
#define SS_GUEST_STDIO_H
#include <stdarg.h>
#include <stddef.h>
typedef struct ss_file FILE;
extern FILE *const stderr;
extern FILE *const stdout;
FILE *fopen(const char *path, const char *mode);
size_t fread(void *buffer, size_t size, size_t count, FILE *file);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *file);
int fclose(FILE *file);
int fflush(FILE *file);
int fprintf(FILE *file, const char *format, ...);
int snprintf(char *out, size_t size, const char *format, ...);
int vsnprintf(char *out, size_t size, const char *format, va_list arguments);
int remove(const char *path);
int rename(const char *from, const char *to);
#endif

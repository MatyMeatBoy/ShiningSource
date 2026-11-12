#ifndef SS_GUEST_STDLIB_H
#define SS_GUEST_STDLIB_H
#include <stddef.h>
char *getenv(const char *name);
unsigned long strtoul(const char *text, char **end, int base);
void exit(int code) __attribute__((noreturn));
int abs(int value);
#endif

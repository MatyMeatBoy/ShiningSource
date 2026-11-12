#ifndef SS_GUEST_STRING_H
#define SS_GUEST_STRING_H
#include <stddef.h>
void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *a, const void *b, size_t size);
size_t strlen(const char *text);
#endif

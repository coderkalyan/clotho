/* Minimal freestanding libc for the Hazard3 CoreMark bare-metal build.
   Supplies the mem/str symbols CoreMark references under -nostdlib. Compile
   with -fno-builtin so these are not rewritten into calls to themselves.
   Char-out and exit route to the tb_hazard3 MMIO block (0xc000_0000). */
#include <stddef.h>

#define IO_PRINT_CHAR 0xc0000000
#define IO_EXIT       0xc0000008

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    while (n--)
        *dp++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && (*d++ = *s++))
        n--;
    while (n--)
        *d++ = 0;
    return r;
}

int puts(const char *s)
{
    while (*s)
        *(volatile int *)IO_PRINT_CHAR = (int)(unsigned char)*s++;
    *(volatile int *)IO_PRINT_CHAR = '\n';
    return 0;
}

void exit(int code)
{
    *(volatile int *)IO_EXIT = code;
    for (;;)
        ;
}
void _exit(int code) { exit(code); }

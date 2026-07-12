/* Minimal freestanding libc for the CoreMark bare-metal RV32 build.
   Replaces the newlib symbols crt0 + CoreMark reference under -nostdlib.
   Compile with -fno-builtin so these don't get rewritten into calls to
   themselves. Exit traps the picorv32 core (ebreak) so the testbench finishes;
   the normal CoreMark success path returns to crt0, which sets the tests-passed
   register before trapping. */
#include <stddef.h>

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

#define STDOUT_REG 0x10000000
int puts(const char *s)
{
    while (*s)
        *(volatile int *)STDOUT_REG = (int)(unsigned char)*s++;
    *(volatile int *)STDOUT_REG = '\n';
    return 0;
}

/* crt0 stubs (we have no static constructors / atexit handlers to run). */
int atexit(void (*fn)(void)) { (void)fn; return 0; }
void __libc_init_array(void) {}
void __libc_fini_array(void) {}

void exit(int code)
{
    (void)code;
    /* picorv32 has no exit peripheral; ebreak traps and lets the testbench end
       (an error exit yields "ERROR!"; the normal success path returns to crt0,
       which sets the tests-passed register first). */
    for (;;)
        __asm__ volatile("ebreak");
}
void _exit(int code) { exit(code); }

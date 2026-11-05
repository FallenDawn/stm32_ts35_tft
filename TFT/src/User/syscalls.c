/*
 * Minimal newlib heap support for the bare-metal STM32F207 target.
 *
 * The firmware uses malloc/free for bitmap and font update buffers.  The
 * previous Cube HAL framework supplied the syscall glue implicitly; the
 * SPL-only build provides _sbrk here and keeps the heap below the reserved
 * stack region defined by the JZ linker script.
 */
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

extern char _end;
extern char _estack;
extern char _Min_Stack_Size;

static char *heap_end;

caddr_t _sbrk(int incr)
{
    uintptr_t current;
    uintptr_t next;
    uintptr_t heap_limit;

    if (heap_end == 0)
        heap_end = &_end;

    current = (uintptr_t)heap_end;
    heap_limit = (uintptr_t)&_estack - (uintptr_t)&_Min_Stack_Size;

    if (current > heap_limit || incr < 0 || (uintptr_t)incr > heap_limit - current)
    {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    next = current + (uintptr_t)incr;
    heap_end = (char *)next;
    return (caddr_t)current;
}

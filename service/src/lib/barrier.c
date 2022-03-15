/*--------------------------------------------------------------------------
 *
 * barrier.c
 *      Atomics and Barrier helpers
 *
 * Implementation of inline ASM, C, and intrinsic test-and-set, barrier, and
 * mutex clearing functions. While the volatile keyword is dubious in compiler
 * implementation, it's used here as a hint to the compiler and reader that
 * the value is critically important to locking mechanisms in the including
 * libraries.
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/barrier.c
 *--------------------------------------------------------------------------
 */
#include "barrier.h"

#if defined(__BUF_NO_ATOMICS__) || defined(__KERNEL_HAS_BARRIERS__)
static INLINE bool _test_and_set( volatile bool * ) ALWAYS_INLINE_FLATTEN;

bool _test_and_set_mutex( volatile bool * mutex )
{
    while( *mutex == true || _test_and_set( mutex ) == true );
    return true;
}

void _fence( void )
{
    __asm__ __volatile__( "" : : : "memory" );
    return;
}

# ifdef __KERNEL_HAS_BARRIERS__
static INLINE bool _test_and_set( volatile bool * mutex )
{
    register bool initial = true;
    initial = READ_ONCE( *mutex );
    WRITE_ONCE( *mutex, 1 );
    return initial;
}

INLINE void _clear_mutex( volatile bool * mutex )
{
    WRITE_ONCE( *mutex, 0 );
    return;
}
# else
INLINE void _clear_mutex( volatile bool * mutex )
{
    *mutex = false;
    __asm__ __volatile__( "" : : : "memory" );
    return;
}
#  ifdef __x86_64__
static INLINE bool _test_and_set( volatile bool * mutex )
{
    register bool _res = true;

    __asm__ __volatile__(
        "    lock          \n"
        "    xchgb   %0,%1 \n"
:       "+q"(_res), "+m"(*mutex)
:       /* no inputs */
:       "memory", "cc"
    );
    return _res;
}
#  elif defined(__i386__)
static INLINE bool _test_and_set( volatile bool * mutex )
{
    register bool _res = true;

    __asm__ __volatile__(
        "    cmpb    $0,%1 \n"
        "    jne     1f    \n"
        "    lock          \n"
        "    xchgb   %0,%1 \n"
        "1:  \n"
:       "+q"(_res), "+m"(*mutex)
:       /* no inputs */
:       "memory", "cc"
    );

    return _res;
}
#  elif defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
// Note: we can probably leverage a better solution involving the PPC HTM (Hardware Transactional Memory)
static INLINE bool _test_and_set( volatile bool * mutex )
{
    bool _t   = false;
    bool _res = false;

    __asm__ __volatile__(
        "    lwarx   %0,0,%3,1 \n"
        "    cmpwi   %0,0      \n"
        "    bne     $+16      \n"
        "    addi    %0,%0,1   \n"
        "    stwcx.  %0,0,%3   \n"
        "    beq     $+12      \n"
        "    li      %1,1      \n"
        "    b       $+12      \n"
        "    lsync             \n"
        "    li      %1,0      \n"
:   "=&b"(_t), "=r"(_res), "+m"(*mutex)
:   "r"(mutex)
:   "memory", "cc"
    );

    return _res;
}
#  else
static INLINE bool _test_and_set( volatile bool * mutex )
{
    register bool initial = true;
    initial = *mutex;
    *mutex = true;

    return initial;
}
#  endif
# endif // __KERNEL_HAS_BARRIERS
#endif // __KERNEL_HAS_BARRIERS__ || __BUFF_NO_ATOMICS__

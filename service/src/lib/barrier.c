#include "barrier.h"
// TODO: Read and understand https://www.kernel.org/doc/Documentation/memory-barriers.txt
// Implement barriers to ensure that these are truely atomic in the case that stdatomic
// is not available

#if defined __BUF_NO_ATOMICS__ || defined __KERNEL_HAS_BARRIERS__
static __inline__ bool _test_and_set( volatile bool * );

bool _test_and_set_mutex( volatile bool * mutex )
{
    while( *mutex == true || _test_and_set( mutex ) == true );
    return true;
}

# ifdef __KERNEL_HAS_BARRIERS__
static __inline__ bool _test_and_set( volatile bool * mutex )
{
    register bool initial = true;
    initial = READ_ONCE( *mutex );
    smp_mb();
    WRITE_ONCE( *mutex, 1 );
    return initial;
}

__inline__ void _clear_mutex( volatile bool * mutex )
{
    WRITE_ONCE( *mutex, 0 );
    smp_mb();
    return;
}
# else
#  ifdef __x86_64__
static __inline__ bool _test_and_set( volatile bool * mutex )
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

__inline__ void _clear_mutex( volatile bool * mutex )
{
    *mutex = false;
    __asm__ __volatile__( "" : : : "memory" );
    return;
}
#  elif defined(__i386__)
static __inline__ bool _test_and_set( volatile bool * mutex )
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

__inline__ void _clear_mutex( volatile bool * mutex )
{
    *mutex = false;
    __asm__ __volatile__( "" : : : "memory" );
    return;
}
#  elif defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
static __inline__ bool _test_and_set( volatile bool * mutex )
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

__inline__ bool _clear_mutex( volatile bool * mutex )
{
    *mutex = false;
    __asm__ __volatile__( "" : : : "memory" );
    return;
}
#  else
static __inline__ bool _test_and_set( volatile bool * mutex )
{
    register bool initial = true;
    initial = *mutex;
    *mutex = true;

    return initial;
}

__inline__ void _clear_mutex( volatile bool * mutex )
{
    *mutex = false;
    return;
}
#  endif
# endif // __KERNEL_HAS_BARRIERS
#endif // __KERNEL_HAS_BARRIERS__ || __BUFF_NO_ATOMICS__

#ifndef _BARRIER_H
#define _BARRIER_H

#include <stdbool.h>
#include <linux/version.h>

#if __STDC_VERSION__ >= 201112L
# ifdef __STDC_NO_ATOMICS__
    // Barriers via syscall introduced in kernel 4.16
#   if LINUX_VERSION_CODE >= KERNEL_VERSION(4,16,0)
#    define __KERNEL_HAS_BARRIERS__
#    include <linux/membarrier.h>
#    include <linux/compiler.h>
#    include <sys/syscall.h>
#   else
#    define __BUF_NO_ATOMICS__
#   endif // 
# else
#  include <stdatomic.h>
#  define __TNS_MUTEX(val) atomic_test_and_set(val)
#  define __C_MUTEX(val) atomic_flag_clear(val)
# endif // __STDC_NO_ATOMICS__
#else
#define __BUF_NO_ATOMICS__
#endif // __STDC_VERSION__

#if defined(__BUF_NO_ATOMICS__) || defined(__KERNEL_HAS_BARRIERS__)
bool _test_and_set_mutex( volatile bool * );
void _clear_mutex( volatile bool * );
#define __TNS_MUTEX(val) _test_and_set_mutex(val)
#define __C_MUTEX(val) _clear_mutex(val)
#endif // __BUF_NO_ATOMICS || __KERNEL_HAS_BARRIERS__
#endif // _BARRIER_H

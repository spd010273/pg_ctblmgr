/*--------------------------------------------------------------------------
 *
 * barrier.h
 *     Atomics and Barrier helpers
 *
 * Provides a simplified interface for memory barriers, atomic test-and-set,
 * and unlocking functions provided by either inline ASM, the compiler's
 * intrinsics (iff available) or a C fallback.
 *
 * For more information, see:
 *  https://www.kernel.org/doc/Documentation/memory-barriers.txt
 *
 * The main exports of this library are:
 *    __TNS_MUTEX( volatile bool * ) - Atomic lock test-and-set
 *    __C_MUTEX( volatile bool * ) - Atomic lock clearing
 *    __FENCE( void ) - basic memory fencing
 *
 * Implementation of each of these is dependent on the system's configuration
 * and architecture capabilities, and thus it's critically important that the
 * binaries not be shipped but rather compiled on their target machine.
 *
 * Copyright (c) 2019-2022, MerchLogix, Inc.
 *
 * IDENTIFICATION
 *          service/src/lib/barrier.h
 *--------------------------------------------------------------------------
 */
#ifndef _BARRIER_H
#define _BARRIER_H

#include <stdbool.h>
#include <linux/version.h>
#include "compiler.h"

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
extern bool _test_and_set_mutex( volatile bool * );
extern void _clear_mutex( volatile bool * );
#define __TNS_MUTEX(val) _test_and_set_mutex(val)
#define __C_MUTEX(val) _clear_mutex(val)
#endif // __BUF_NO_ATOMICS || __KERNEL_HAS_BARRIERS__
extern void _fence( void );
#define __FENCE() _fence()
#endif // _BARRIER_H

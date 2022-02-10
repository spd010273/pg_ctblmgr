/*------------------------------------------------------------------------
 *
 * compiler.h
 *     Abstracts compiler function attributes

 * Copyright (c) 2022, MerchLogix Inc.
 *
 * IDENTIFICATION
 *        service/src/lib/compiler.h
 *
 *------------------------------------------------------------------------
 */

#ifndef _COMPILER_H
#define _COMPILER_H

#ifdef __GNUC__
#define INLINE __inline__
#define ALWAYS_INLINE_FLATTEN_HOT __attribute__((always_inline, flatten, hot))
#define ALWAYS_INLINE __attribute__((always_inline))
#define ALWAYS_INLINE_FLATTEN __attribute__((always_inline, flatten))
#define FLATTEN_HOT __attribute__((flatten, hot))
#define PACKED __attribute__((packed))
#define PRINTF __attribute((format (gnu_printf, 2, 3) ))
#define FLATTEN __attribute__((flatten))
#define UNUSED __attribute__((unused))
#elif defined( __clang__ ) && defined( __llvm__ )
#define INLINE 
#define ALWAYS_INLINE_FLATTEN_HOT alwaysinline hot
#define ALWAYS_INLINE alwaysinline
#define ALWAYS_INLINE_FLATTEN alwaysinline
#define FLATTEN_HOT hot
#else
#define INLINE inline
#endif // __GNUC__
#endif // _COMPILER_H

#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef PRNE_DEBUG
#include <stdio.h>
#include <errno.h>
#endif

#define PRNE_LIMIT_ENUM(t,x,l) _Static_assert((x) <= (l),"enum overflow: "#t)

#define prne_op_min(a, b) ((a) < (b) ? (a) : (b))
#define prne_op_max(a, b) ((a) > (b) ? (a) : (b))
#define prne_op_spaceship(a, b) ((a) == (b) ? 0 : (a) < (b) ? -1 : 1)

#define prne_salign_next(x, align) (((x) % align == 0) ? (x) : ((x) / align + 1) * align)
#define prne_salign_at(x, align) (((x) % align == 0) ? (x) : ((x) / align) * align)

#if !defined(memzero)
#define memzero(addr, len) memset(addr, 0, len)
#endif

#ifdef PRNE_DEBUG
#define prne_dbgpf(...) fprintf(stderr, __VA_ARGS__)
#define prne_dbgperr(str) perror(str)
#define prne_assert(expr) assert(expr)
#define prne_massert(expr, ...)\
	if (!(expr)) {\
		fprintf(stderr, "*** ");\
		fprintf(stderr, __VA_ARGS__);\
		fprintf(stderr, "\n");\
		abort();\
	}
#define prne_dbgast(expr) prne_assert(expr)
#define prne_dbgmast(expr, ...) prne_massert(expr, __VA_ARGS__)
#else
#define prne_dbgpf(...)
#define prne_dbgperr(str)
#define prne_assert(expr)\
	if (!(expr)) {\
		abort();\
	}
#define prne_massert(expr, ...) prne_assert(expr)
#define prne_dbgast(expr)
#define prne_dbgmast(expr, ...)
#endif

/**********************************************************************
* Endianess Independent Byte Extraction
***********************************************************************/
/* prne_getmsbN(x, n)
*
* Extract nth most significant byte of x.
*/
#define prne_getmsb(x, n, w, s)\
	(uint8_t)(((w)(x) & (w)0xFF << (s - 8 * (n))) >> (s - 8 * (n)))
#define prne_getmsb64(x, n) prne_getmsb((x), (n), uint_fast64_t, 56)
#define prne_getmsb32(x, n) prne_getmsb((x), (n), uint_fast32_t, 24)
#define prne_getmsb16(x, n) prne_getmsb((x), (n), uint_fast16_t, 8)

/* prne_recmb_msbN(...)
*
* Recombine bytes in big-endian order to uintN.
*/
#define prne_recmb_msb64(a, b, c, d, e, f, g, h) (\
	((uint_fast64_t)(a) << 56) |\
	((uint_fast64_t)(b) << 48) |\
	((uint_fast64_t)(c) << 40) |\
	((uint_fast64_t)(d) << 32) |\
	((uint_fast64_t)(e) << 24) |\
	((uint_fast64_t)(f) << 16) |\
	((uint_fast64_t)(g) << 8) |\
	((uint_fast64_t)(h) << 0)\
)
#define prne_recmb_msb32(a, b, c, d) (\
	((uint_fast32_t)(a) << 24) |\
	((uint_fast32_t)(b) << 16) |\
	((uint_fast32_t)(c) << 8) |\
	((uint_fast32_t)(d) << 0)\
)
#define prne_recmb_msb16(a, b) (\
	((uint_fast16_t)(a) << 8) |\
	((uint_fast16_t)(b) << 0)\
)

/* Machine Characteristics
*/
#define PRNE_ENDIAN_LITTLE 1
#define PRNE_ENDIAN_BIG 2

#ifdef __GNUC__
	#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		#define PRNE_HOST_ENDIAN PRNE_ENDIAN_BIG
	#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		#define PRNE_HOST_ENDIAN PRNE_ENDIAN_LITTLE
	#else
		#error "FIXME!"
	#endif
#else
	#error "FIXME!"
#endif

#define prne_einv16(x) (((0xFF00 & x) >> 8) | ((0x00FF & x) << 8))

#if PRNE_HOST_ENDIAN == PRNE_ENDIAN_BIG
#define prne_htobe16(x) (x)
#define prne_be16toh(x) (x)
#elif PRNE_HOST_ENDIAN == PRNE_ENDIAN_LITTLE
#define prne_htobe16(x) prne_einv16(x)
#define prne_be16toh(x) prne_einv16(x)
#else
#endif

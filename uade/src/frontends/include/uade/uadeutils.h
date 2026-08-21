#ifndef _UADE_UTILS_H_
#define _UADE_UTILS_H_

#include <stdint.h>
#include <stdio.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define UADE_HOST_BIG_ENDIAN 1
#else
#define UADE_HOST_BIG_ENDIAN 0
#endif

/*
 * Host <-> big-endian conversion for the wire format. Self-inverse, so one
 * function covers what hton/ntoh needed a pair for. Hand-rolled because the
 * Winsock versions would drag ws2_32 back onto the link line.
 */
static inline uint32_t uade_be32(uint32_t x)
{
#if UADE_HOST_BIG_ENDIAN
	return x;
#else
	return ((x & 0xffu) << 24) | ((x & 0xff00u) << 8) |
	       ((x >> 8) & 0xff00u) | ((x >> 24) & 0xffu);
#endif
}

static inline uint16_t uade_be16(uint16_t x)
{
#if UADE_HOST_BIG_ENDIAN
	return x;
#else
	return (uint16_t) ((x << 8) | (x >> 8));
#endif
}

static inline uint16_t read_be_u16(void *s)
{
	uint16_t x;
	uint8_t *ptr = (uint8_t *) s;
	x = ptr[1] + (ptr[0] << 8);
	return x;
}

static inline uint32_t read_be_u32(void *s)
{
	uint32_t x;
	uint8_t *ptr = (uint8_t *) s;
	x = (ptr[0] << 24) + (ptr[1] << 16) + (ptr[2] << 8) + ptr[3];
	return x;
}

static inline void write_be_u32(void *s, uint32_t x)
{
	uint8_t *ptr = (uint8_t *) s;
	ptr[0] = (x >> 24);
	ptr[1] = (x >> 16);
	ptr[2] = (x >> 8);
	ptr[3] = x;
}

static inline void write_be_u16(void *s, uint16_t x)
{
	uint8_t *ptr = (uint8_t *) s;
	ptr[0] = (x >> 8);
	ptr[1] = x;
}

static inline void write_be_s16(void *s, int16_t x)
{
	write_be_u16(s, (uint16_t) x);
}

#endif

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef long ssize_t;

typedef u64 pte_t;
typedef u64 *pagetable_t;   // 512 PTEs

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ROUNDUP(x, a) (((x) + (a) - 1) & ~((u64)(a) - 1))
#define ROUNDDOWN(x, a) ((x) & ~((u64)(a) - 1))
#define container_of(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __packed __attribute__((packed))
#define __noreturn __attribute__((noreturn))

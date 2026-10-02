#pragma once
/*
 * ps4_kstat.h, PS5 edition. The payload SDK's <sys/stat.h> is the FreeBSD
 * header the PS5 kernel was built from, so `struct stat` already is the
 * kernel's layout and the PS4 port's mirror struct is not needed. The names
 * stay so the shared console code compiles unchanged.
 */
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
struct Ps4KernelStat : public ::stat {};

static inline int ps4KernelFstat(int fd, struct Ps4KernelStat* out) { return ::fstat(fd, out); }
static inline int ps4KernelStat(const char* path, struct Ps4KernelStat* out) { return ::stat(path, out); }
#endif

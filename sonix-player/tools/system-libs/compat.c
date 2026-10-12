// Entry points that the glibc 2.39 headers name and the device's glibc 2.22
// does not export. Built once per function (-DF_<name>), so that an archive
// member is pulled in only where it is called. Hidden, so a shared library
// does not export them.
//
// - stat and friends: 2.22 exports only __xstat and the rest; the plain names
//   were wrappers in its libc_nonshared.a.
// - fcntl64: the 2.28 name that _FILE_OFFSET_BITS=64 redirects fcntl to.
// - __isoc23_*: the 2.38 names _GNU_SOURCE redirects strtol and scanf to.
//   They differ from the C99 ones only in taking "0b" prefixes.
//
// Built without _GNU_SOURCE, so the plain names below are the C99 ones.

#define _LARGEFILE64_SOURCE
#define _ATFILE_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wchar.h>

#define HIDDEN __attribute__((visibility("hidden")))
#define STAT_VER 3 // _STAT_VER_LINUX on MIPS o32

extern int __xstat(int, const char *, struct stat *);
extern int __lxstat(int, const char *, struct stat *);
extern int __fxstat(int, int, struct stat *);
extern int __fxstatat(int, int, const char *, struct stat *, int);
extern int __xstat64(int, const char *, struct stat64 *);
extern int __lxstat64(int, const char *, struct stat64 *);
extern int __fxstat64(int, int, struct stat64 *);
extern int __fxstatat64(int, int, const char *, struct stat64 *, int);
extern int __xmknod(int, const char *, mode_t, dev_t *);
extern int __xmknodat(int, int, const char *, mode_t, dev_t *);
extern int fcntl_222(int, int, ...) __asm__("fcntl");

#ifdef F_stat
HIDDEN int stat(const char *p, struct stat *b) { return __xstat(STAT_VER, p, b); }
#endif
#ifdef F_lstat
HIDDEN int lstat(const char *p, struct stat *b) { return __lxstat(STAT_VER, p, b); }
#endif
#ifdef F_fstat
HIDDEN int fstat(int fd, struct stat *b) { return __fxstat(STAT_VER, fd, b); }
#endif
#ifdef F_fstatat
HIDDEN int fstatat(int d, const char *p, struct stat *b, int f) { return __fxstatat(STAT_VER, d, p, b, f); }
#endif
#ifdef F_stat64
HIDDEN int stat64(const char *p, struct stat64 *b) { return __xstat64(STAT_VER, p, b); }
#endif
#ifdef F_lstat64
HIDDEN int lstat64(const char *p, struct stat64 *b) { return __lxstat64(STAT_VER, p, b); }
#endif
#ifdef F_fstat64
HIDDEN int fstat64(int fd, struct stat64 *b) { return __fxstat64(STAT_VER, fd, b); }
#endif
#ifdef F_fstatat64
HIDDEN int fstatat64(int d, const char *p, struct stat64 *b, int f) { return __fxstatat64(STAT_VER, d, p, b, f); }
#endif
#ifdef F_mknod
HIDDEN int mknod(const char *p, mode_t m, dev_t d) { return __xmknod(0, p, m, &d); }
#endif
#ifdef F_mknodat
HIDDEN int mknodat(int fd, const char *p, mode_t m, dev_t d) { return __xmknodat(0, fd, p, m, &d); }
#endif

#ifdef F_fcntl64
HIDDEN int fcntl64(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	return fcntl_222(fd, cmd, arg);
}
#endif

#ifdef F___isoc23_strtol
HIDDEN long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
#endif
#ifdef F___isoc23_strtoul
HIDDEN unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
#endif
#ifdef F___isoc23_strtoll
HIDDEN long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
#endif
#ifdef F___isoc23_strtoull
HIDDEN unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
#endif
#ifdef F___isoc23_strtoimax
HIDDEN intmax_t __isoc23_strtoimax(const char *s, char **e, int b) { return strtoimax(s, e, b); }
#endif
#ifdef F___isoc23_strtoumax
HIDDEN uintmax_t __isoc23_strtoumax(const char *s, char **e, int b) { return strtoumax(s, e, b); }
#endif
#ifdef F___isoc23_wcstol
HIDDEN long __isoc23_wcstol(const wchar_t *s, wchar_t **e, int b) { return wcstol(s, e, b); }
#endif
#ifdef F___isoc23_wcstoul
HIDDEN unsigned long __isoc23_wcstoul(const wchar_t *s, wchar_t **e, int b) { return wcstoul(s, e, b); }
#endif
#ifdef F___isoc23_wcstoll
HIDDEN long long __isoc23_wcstoll(const wchar_t *s, wchar_t **e, int b) { return wcstoll(s, e, b); }
#endif
#ifdef F___isoc23_wcstoull
HIDDEN unsigned long long __isoc23_wcstoull(const wchar_t *s, wchar_t **e, int b) { return wcstoull(s, e, b); }
#endif
#ifdef F___isoc23_vsscanf
HIDDEN int __isoc23_vsscanf(const char *s, const char *f, va_list ap) { return vsscanf(s, f, ap); }
#endif
#ifdef F___isoc23_vfscanf
HIDDEN int __isoc23_vfscanf(FILE *fp, const char *f, va_list ap) { return vfscanf(fp, f, ap); }
#endif
#ifdef F___isoc23_vscanf
HIDDEN int __isoc23_vscanf(const char *f, va_list ap) { return vscanf(f, ap); }
#endif
#ifdef F___isoc23_sscanf
HIDDEN int __isoc23_sscanf(const char *s, const char *f, ...) {
	va_list ap;
	va_start(ap, f);
	int r = vsscanf(s, f, ap);
	va_end(ap);
	return r;
}
#endif
#ifdef F___isoc23_fscanf
HIDDEN int __isoc23_fscanf(FILE *fp, const char *f, ...) {
	va_list ap;
	va_start(ap, f);
	int r = vfscanf(fp, f, ap);
	va_end(ap);
	return r;
}
#endif
#ifdef F___isoc23_scanf
HIDDEN int __isoc23_scanf(const char *f, ...) {
	va_list ap;
	va_start(ap, f);
	int r = vscanf(f, ap);
	va_end(ap);
	return r;
}
#endif

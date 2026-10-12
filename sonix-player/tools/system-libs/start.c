// The crt1.o of glibc 2.34 and later passes no init function to
// __libc_start_main and expects libc to run the executable's constructors;
// the device's glibc 2.22 runs only the function it is passed. This passes it
// the one that 2.22's own crt1.o and libc_nonshared.a would have: the
// preinit array, _init, then the init array. Linked into every executable
// with -Wl,--wrap=__libc_start_main.

#include <stddef.h>

typedef void (*init_fn)(int, char **, char **);

extern init_fn __preinit_array_start[] __attribute__((visibility("hidden")));
extern init_fn __preinit_array_end[] __attribute__((visibility("hidden")));
extern init_fn __init_array_start[] __attribute__((visibility("hidden")));
extern init_fn __init_array_end[] __attribute__((visibility("hidden")));
extern void _init(void);

static void csu_init(int argc, char **argv, char **envp) {
	size_t n = __preinit_array_end - __preinit_array_start;
	for (size_t i = 0; i < n; i++) {
		__preinit_array_start[i](argc, argv, envp);
	}
	_init();
	n = __init_array_end - __init_array_start;
	for (size_t i = 0; i < n; i++) {
		__init_array_start[i](argc, argv, envp);
	}
}

extern int __real___libc_start_main(int (*)(int, char **, char **), int, char **, init_fn, void (*)(void),
				    void (*)(void), void *);

// The executable's destructors are run by ld.so, as on 2.22, so fini is NULL.
int __wrap___libc_start_main(int (*main)(int, char **, char **), int argc, char **argv, init_fn init,
			     void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
	(void)init;
	(void)fini;
	return __real___libc_start_main(main, argc, argv, csu_init, NULL, rtld_fini, stack_end);
}

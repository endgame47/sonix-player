// sigset() is exported by the device's glibc but no longer declared by the
// newer headers. Force-included into thttpd.
#include <signal.h>
extern __sighandler_t sigset(int, __sighandler_t);

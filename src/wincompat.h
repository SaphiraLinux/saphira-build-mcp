#ifndef SBM_WINCOMPAT_H
#define SBM_WINCOMPAT_H

/* Small Windows compatibility shims for the MinGW-w64 cross build.
 * On non-Windows builds this header contributes nothing: the POSIX
 * APIs are used directly. The shims cover only what the server
 * needs — no portability framework, no behaviour change anywhere
 * the native API exists.
 *
 * Deliberately NOT wrapped here (handled at use sites instead):
 * fork/exec/wait (no Windows equivalent in this codebase — the
 * subprocess paths fail closed on Windows), symlinks, file modes. */

#include <time.h>

#ifdef _WIN32

#include <direct.h>
#include <fcntl.h>
#include <io.h>

/* POSIX names used by this codebase, mapped to the CRT spellings.
 * MinGW also accepts most of these directly under -std=gnu11, but
 * the explicit mapping documents the dependency instead of
 * relying on deprecated aliases. */
#define SBM_READ _read
#define SBM_WRITE _write
#define SBM_GETCWD _getcwd
#define SBM_MKDIR(p, m) _mkdir(p)
#define SBM_ACCESS(p, m) _access(p, m)

/* O_BINARY exists in <fcntl.h>; X_OK has no Windows meaning, and
 * executability is established by attempting the spawn, never by
 * probing bits. */
#ifndef X_OK
#define X_OK 0
#endif

#else /* POSIX */

#include <unistd.h>

#define SBM_READ read
#define SBM_WRITE write
#define SBM_GETCWD getcwd
#define SBM_MKDIR(p, m) mkdir(p, m)
#define SBM_ACCESS(p, m) access(p, m)

#endif /* _WIN32 */

/* gmtime_r is hidden under -std=gnu11 on MinGW-w64; gmtime_s is
 * always present there. Same contract both ways (UTC breakdown of
 * t into tm); callers keep ignoring failure exactly as before. */
static inline void sb_gmtime(const time_t *t, struct tm *tm) {
#ifdef _WIN32
	(void)gmtime_s(tm, t);
#else
	(void)gmtime_r(t, tm);
#endif
}

#endif

#include "runner.h"
#include "db.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void utc_now(char *out, size_t n) {
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static long elapsed_ms(struct timespec a, struct timespec b) {
	long s = (long)(b.tv_sec - a.tv_sec);
	long ns = (long)(b.tv_nsec - a.tv_nsec);
	return s * 1000 + ns / 1000000;
}

/* Best-effort audit row. A failed audit never fails the tool call:
 * the failure is reported on stderr, the result stands. */
static void audit(const ext_tool *t, const char *author,
                  const char *recorder, int exit_status, long ms,
                  int truncated, int timed_out, const char *argv_text) {
	sqlite3 *db;
	sqlite3_stmt *st = NULL;
	char err[256];
	char ts[32];
	const char *sql =
	    "INSERT INTO tool_runs(tool,argv,class,author,recorder,"
	    "exit_status,elapsed_ms,truncated,timed_out,created_at)"
	    "VALUES(?,?,?,?,?,?,?,?,?,?)";
	db = db_open_notes(err, sizeof err);
	if (!db) {
		fprintf(stderr, "sbm: tool_runs audit skipped: %s\n", err);
		return;
	}
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "sbm: tool_runs audit skipped: %s\n",
		        sqlite3_errmsg(db));
		sqlite3_close(db);
		return;
	}
	utc_now(ts, sizeof ts);
	sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, argv_text, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, t->cls, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 4, author ? author : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, recorder ? recorder : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 6, exit_status);
	sqlite3_bind_int64(st, 7, (sqlite3_int64)ms);
	sqlite3_bind_int(st, 8, truncated);
	sqlite3_bind_int(st, 9, timed_out);
	sqlite3_bind_text(st, 10, ts, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) != SQLITE_DONE)
		fprintf(stderr, "sbm: tool_runs audit skipped: %s\n",
		        sqlite3_errmsg(db));
	sqlite3_finalize(st);
	sqlite3_close(db);
}

static const char *bwrap_bin(void) {
	return env_or("SBM_BWRAP", "bwrap");
}

int runner_exec(const ext_tool *t, const char *author,
                const char *recorder, gbuf *out, int *is_error) {
	int pipefd[2] = { -1, -1 };
	pid_t pid;
	struct timespec t0, now;
	struct stat st;
	gbuf argv_text;
	char *exec_argv[CONF_MAX_ARGV + 32];
	int ea = 0;
	int status = -1, timed_out = 0, truncated = 0, reaped = 0;
	long ms = 0;
	char foot[128];
	static const char *sandbox_pre[] = {
		"--unshare-pid", "--unshare-uts", "--unshare-ipc",
		"--unshare-net", "--die-with-parent",
		"--ro-bind", "/bin", "/bin",
		"--ro-bind", "/sbin", "/sbin",
		"--ro-bind", "/usr", "/usr",
		"--ro-bind", "/lib", "/lib",
		"--ro-bind", "/recipes", "/recipes",
		"--tmpfs", "/tmp",
	};
	static const char *child_env[] = {
		"PATH=/bin:/sbin:/usr/bin:/usr/sbin",
		"PAGER=cat",
		"GIT_PAGER=cat",
		"GIT_CONFIG_NOSYSTEM=1",
		NULL
	};

	*is_error = 0;
	gbuf_init(&argv_text);
	/* Registered executable must be a live regular file: the conf
	 * was curated, the filesystem is truth. */
	if (stat(t->exe, &st) != 0 || !S_ISREG(st.st_mode) ||
	    access(t->exe, X_OK) != 0) {
		gbuf_adds(out, "tool executable unavailable");
		*is_error = 1;
		audit(t, author, recorder, -1, 0, 0, 0, t->exe);
		return 0;
	}
	if (stat(t->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		gbuf_adds(out, "tool working directory unavailable");
		*is_error = 1;
		audit(t, author, recorder, -1, 0, 0, 0, t->exe);
		return 0;
	}
	if (pipe(pipefd) != 0) {
		gbuf_adds(out, "internal error: pipe failed");
		*is_error = 1;
		return 0;
	}
	{
		int fl = fcntl(pipefd[0], F_GETFL, 0);
		if (fl >= 0)
			fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
	}
	if (t->sandbox) {
		size_t i;
		exec_argv[ea++] = (char *)bwrap_bin();
		for (i = 0; i < sizeof sandbox_pre / sizeof sandbox_pre[0]; i++)
			exec_argv[ea++] = (char *)sandbox_pre[i];
		if (strcmp(t->dir, "/") != 0 &&
		    strcmp(t->dir, "/recipes") != 0) {
			exec_argv[ea++] = "--ro-bind";
			exec_argv[ea++] = t->dir;
			exec_argv[ea++] = t->dir;
		}
		exec_argv[ea++] = "--chdir";
		exec_argv[ea++] = t->dir;
		exec_argv[ea++] = "--";
	}
	for (status = 0; status < t->argc; status++)
		exec_argv[ea++] = t->argv[status];
	exec_argv[ea] = NULL;
	status = -1;
	/* argv text for the audit row (fixed conf argv, space-joined). */
	for (ea = 0; ea < t->argc; ea++) {
		if (ea)
			gbuf_addch(&argv_text, ' ');
		gbuf_adds(&argv_text, t->argv[ea]);
	}
	pid = fork();
	if (pid < 0) {
		gbuf_adds(out, "internal error: fork failed");
		*is_error = 1;
		close(pipefd[0]);
		close(pipefd[1]);
		gbuf_free(&argv_text);
		return 0;
	}
	if (pid == 0) {
		/* Child: no shell, no PATH search for the tool itself.
		 * Combined stdout+stderr so test failures are visible
		 * in one place. */
		int i;
		close(pipefd[0]);
		if (dup2(pipefd[1], 1) < 0 || dup2(pipefd[1], 2) < 0)
			_exit(127);
		close(pipefd[1]);
		if (!t->sandbox && chdir(t->dir) != 0)
			_exit(127);
		if (t->sandbox)
			execvp(exec_argv[0], exec_argv);
		else {
			char *vec[CONF_MAX_ARGV + 1];
			for (i = 0; i < t->argc; i++)
				vec[i] = t->argv[i];
			vec[t->argc] = NULL;
			execve(t->exe, vec, (char *const *)child_env);
		}
		_exit(127);
	}
	close(pipefd[1]);
	pipefd[1] = -1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		char buf[8192];
		ssize_t n;
		pid_t w = 0;
		int drained = 0;
		n = read(pipefd[0], buf, sizeof buf);
		if (n > 0) {
			if ((long)(out->len) < t->max_bytes) {
				size_t room = (size_t)(t->max_bytes - (long)out->len);
				gbuf_addn(out, buf, (size_t)n < room ? (size_t)n : room);
				if ((size_t)n > room)
					truncated = 1;
			} else {
				truncated = 1;
			}
		} else if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
			/* EOF (writers gone) or nothing available now. */
			drained = (n == 0);
			if (!reaped)
				w = waitpid(pid, &status, WNOHANG);
		} else {
			break; /* unexpected read error: stop with what we have */
		}
		if (!reaped && w == 0)
			w = waitpid(pid, &status, WNOHANG);
		if (w == pid)
			reaped = 1;
		if (reaped && (drained || n == 0))
			break;
		clock_gettime(CLOCK_MONOTONIC, &now);
		ms = elapsed_ms(t0, now);
		if (ms > t->timeout_s * 1000L) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			timed_out = 1;
			status = -1;
			break;
		}
		if (n <= 0)
			usleep(20000);
	}
	/* Final drain: the child is reaped but bytes may remain. */
	for (;;) {
		char buf[8192];
		ssize_t n = read(pipefd[0], buf, sizeof buf);
		if (n > 0) {
			if ((long)(out->len) < t->max_bytes) {
				size_t room = (size_t)(t->max_bytes - (long)out->len);
				gbuf_addn(out, buf, (size_t)n < room ? (size_t)n : room);
				if ((size_t)n > room)
					truncated = 1;
			} else {
				truncated = 1;
			}
			continue;
		}
		break;
	}
	close(pipefd[0]);
	clock_gettime(CLOCK_MONOTONIC, &now);
	{
		int code;
		if (timed_out) {
			gbuf_adds(out, out->len ? "\n[timeout]" : "[timeout]");
			*is_error = 1;
			code = -1;
		} else if (WIFEXITED(status)) {
			code = WEXITSTATUS(status);
			if (code != 0)
				*is_error = 1;
		} else {
			code = -1;
			*is_error = 1;
		}
		if (truncated)
			gbuf_adds(out, "\n[truncated]");
		snprintf(foot, sizeof foot, "\n[exit=%d elapsed_ms=%ld]", code, ms);
		gbuf_adds(out, foot);
		audit(t, author, recorder, code, ms, truncated, timed_out,
		      argv_text.p ? argv_text.p : "");
	}
	gbuf_free(&argv_text);
	return 0;
}

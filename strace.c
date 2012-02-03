/*
 * Copyright (c) 1991, 1992 Paul Kranenburg <pk@cs.few.eur.nl>
 * Copyright (c) 1993 Branko Lankester <branko@hacktic.nl>
 * Copyright (c) 1993, 1994, 1995, 1996 Rick Sladkey <jrs@world.std.com>
 * Copyright (c) 1996-1999 Wichert Akkerman <wichert@cistron.nl>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *	$Id$
 */

#include "defs.h"

#include <sys/types.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <sys/param.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <dirent.h>
#include <sys/utsname.h>

#ifdef LINUX
# include <asm/unistd.h>
# if defined __NR_tkill
#  define my_tkill(tid, sig) syscall(__NR_tkill, (tid), (sig))
# else
   /* kill() may choose arbitrarily the target task of the process group
      while we later wait on a that specific TID.  PID process waits become
      TID task specific waits for a process under ptrace(2).  */
#  warning "Neither tkill(2) nor tgkill(2) available, risk of strace hangs!"
#  define my_tkill(tid, sig) kill((tid), (sig))
# endif
#endif

#if defined(IA64) && defined(LINUX)
# include <asm/ptrace_offsets.h>
#endif

#ifdef USE_PROCFS
#include <poll.h>
#endif

#ifdef SVR4
#include <sys/stropts.h>
#ifdef HAVE_MP_PROCFS
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#endif
#endif
extern char **environ;
extern int optind;
extern char *optarg;


int debug = 0, followfork = 0;
unsigned int ptrace_setoptions = 0;
/* Which WSTOPSIG(status) value marks syscall traps? */
static unsigned int syscall_trap_sig = SIGTRAP;
int dtime = 0, xflag = 0, qflag = 0;
cflag_t cflag = CFLAG_NONE;
static int iflag = 0, pflag_seen = 0, rflag = 0, tflag = 0;

/* -I n */
enum {
    INTR_NOT_SET        = 0,
    INTR_ANYWHERE       = 1, /* don't block/ignore any signals */
    INTR_WHILE_WAIT     = 2, /* block fatal signals while decoding syscall. default */
    INTR_NEVER          = 3, /* block fatal signals. default if '-o FILE PROG' */
    INTR_BLOCK_TSTP_TOO = 4, /* block fatal signals and SIGTSTP (^Z) */
    NUM_INTR_OPTS
};
static int opt_intr;
/* We play with signal mask only if this mode is active: */
#define interactive (opt_intr == INTR_WHILE_WAIT)

/*
 * daemonized_tracer supports -D option.
 * With this option, strace forks twice.
 * Unlike normal case, with -D *grandparent* process exec's,
 * becoming a traced process. Child exits (this prevents traced process
 * from having children it doesn't expect to have), and grandchild
 * attaches to grandparent similarly to strace -p PID.
 * This allows for more transparent interaction in cases
 * when process and its parent are communicating via signals,
 * wait() etc. Without -D, strace process gets lodged in between,
 * disrupting parent<->child link.
 */
static bool daemonized_tracer = 0;

#ifdef USE_SEIZE
static int post_attach_sigstop = TCB_IGNORE_ONE_SIGSTOP;
# define use_seize (post_attach_sigstop == 0)
#else
# define post_attach_sigstop TCB_IGNORE_ONE_SIGSTOP
# define use_seize 0
#endif

/* Sometimes we want to print only succeeding syscalls. */
int not_failing_only = 0;

/* Show path associated with fd arguments */
int show_fd_path = 0;

/* are we filtering traces based on paths? */
int tracing_paths = 0;

static int exit_code = 0;
static int strace_child = 0;
static int strace_tracer_pid = 0;

static char *username = NULL;
static uid_t run_uid;
static gid_t run_gid;

int max_strlen = DEFAULT_STRLEN;
static int acolumn = DEFAULT_ACOLUMN;
static char *acolumn_spaces;
static char *outfname = NULL;
static FILE *outf;
struct tcb *printing_tcp = NULL;
static int curcol;
static struct tcb **tcbtab;
static unsigned int nprocs, tcbtabsize;
static const char *progname;

static char *os_release; /* from uname() */

static int detach(struct tcb *tcp);
static int trace(void);
static void cleanup(void);
static void interrupt(int sig);
static sigset_t empty_set, blocked_set;

#ifdef HAVE_SIG_ATOMIC_T
static volatile sig_atomic_t interrupted;
#else
static volatile int interrupted;
#endif

#ifdef USE_PROCFS

static struct tcb *pfd2tcb(int pfd);
static void reaper(int sig);
static void rebuild_pollv(void);
static struct pollfd *pollv;

#ifndef HAVE_POLLABLE_PROCFS

static void proc_poll_open(void);
static void proc_poller(int pfd);

struct proc_pollfd {
	int fd;
	int revents;
	int pid;
};

static int poller_pid;
static int proc_poll_pipe[2] = { -1, -1 };

#endif /* !HAVE_POLLABLE_PROCFS */

#ifdef HAVE_MP_PROCFS
#define POLLWANT	POLLWRNORM
#else
#define POLLWANT	POLLPRI
#endif
#endif /* USE_PROCFS */

static void
usage(FILE *ofp, int exitval)
{
	fprintf(ofp, "\
usage: strace [-CdDffhiqrtttTvVxxy] [-I n] [-a column] [-e expr]... [-o file]\n\
              [-p pid]... [-s strsize] [-u username] [-E var=val]...\n\
              [-P path] [PROG [ARGS]]\n\
   or: strace -c [-D] [-I n] [-e expr]... [-O overhead] [-S sortby] [-E var=val]...\n\
              [PROG [ARGS]]\n\
-c -- count time, calls, and errors for each syscall and report summary\n\
-C -- like -c but also print regular output while processes are running\n\
-D -- run tracer process as a detached grandchild, not as parent\n\
-f -- follow forks, -ff -- with output into separate files\n\
-F -- attempt to follow vforks\n\
-i -- print instruction pointer at time of syscall\n\
-I interruptible\n\
   1: no signals are blocked\n\
   2: fatal signals are blocked while decoding syscall (default)\n\
   3: fatal signals are always blocked (default if '-o FILE PROG')\n\
   4: fatal signals and SIGTSTP (^Z) are always blocked\n\
      (useful to make 'strace -o FILE PROG' not stop on ^Z)\n\
-q -- suppress messages about attaching, detaching, etc.\n\
-r -- print relative timestamp, -t -- absolute timestamp, -tt -- with usecs\n\
-T -- print time spent in each syscall\n\
-v -- verbose mode: print unabbreviated argv, stat, termios, etc. args\n\
-x -- print non-ascii strings in hex, -xx -- print all strings in hex\n\
-y -- print paths associated with file descriptor arguments\n\
-h -- print help message\n\
-V -- print version\n\
-a column -- alignment COLUMN for printing syscall results (default %d)\n\
-e expr -- a qualifying expression: option=[!]all or option=[!]val1[,val2]...\n\
   options: trace, abbrev, verbose, raw, signal, read, or write\n\
-o file -- send trace output to FILE instead of stderr\n\
-O overhead -- set overhead for tracing syscalls to OVERHEAD usecs\n\
-p pid -- trace process with process id PID, may be repeated\n\
-s strsize -- limit length of print strings to STRSIZE chars (default %d)\n\
-S sortby -- sort syscall counts by: time, calls, name, nothing (default %s)\n\
-u username -- run command as username handling setuid and/or setgid\n\
-E var=val -- put var=val in the environment for command\n\
-E var -- remove var from the environment for command\n\
-P path -- trace accesses to path\n\
" /* this is broken, so don't document it
-z -- print only succeeding syscalls\n\
  */
, DEFAULT_ACOLUMN, DEFAULT_STRLEN, DEFAULT_SORTBY);
	exit(exitval);
}

static void die(void) __attribute__ ((noreturn));
static void die(void)
{
	if (strace_tracer_pid == getpid()) {
		cflag = 0;
		cleanup();
	}
	exit(1);
}

static void verror_msg(int err_no, const char *fmt, va_list p)
{
	char *msg;

	fflush(NULL);

	/* We want to print entire message with single fprintf to ensure
	 * message integrity if stderr is shared with other programs.
	 * Thus we use vasprintf + single fprintf.
	 */
	msg = NULL;
	if (vasprintf(&msg, fmt, p) >= 0) {
		if (err_no)
			fprintf(stderr, "%s: %s: %s\n", progname, msg, strerror(err_no));
		else
			fprintf(stderr, "%s: %s\n", progname, msg);
		free(msg);
	} else {
		/* malloc in vasprintf failed, try it without malloc */
		fprintf(stderr, "%s: ", progname);
		vfprintf(stderr, fmt, p);
		if (err_no)
			fprintf(stderr, ": %s\n", strerror(err_no));
		else
			putc('\n', stderr);
	}
	/* We don't switch stderr to buffered, thus fprintf(stderr)
	 * always flushes its output and this is not necessary: */
	/* fflush(stderr); */
}

void error_msg(const char *fmt, ...)
{
	va_list p;
	va_start(p, fmt);
	verror_msg(0, fmt, p);
	va_end(p);
}

void error_msg_and_die(const char *fmt, ...)
{
	va_list p;
	va_start(p, fmt);
	verror_msg(0, fmt, p);
	die();
}

void perror_msg(const char *fmt, ...)
{
	va_list p;
	va_start(p, fmt);
	verror_msg(errno, fmt, p);
	va_end(p);
}

void perror_msg_and_die(const char *fmt, ...)
{
	va_list p;
	va_start(p, fmt);
	verror_msg(errno, fmt, p);
	die();
}

void die_out_of_memory(void)
{
	static bool recursed = 0;
	if (recursed)
		exit(1);
	recursed = 1;
	error_msg_and_die("Out of memory");
}

#ifdef SVR4
#ifdef MIPS
void
foobar()
{
}
#endif /* MIPS */
#endif /* SVR4 */

/* Glue for systems without a MMU that cannot provide fork() */
#ifdef HAVE_FORK
# define strace_vforked 0
#else
# define strace_vforked 1
# define fork()         vfork()
#endif

#ifdef USE_SEIZE
static int
ptrace_attach_or_seize(int pid)
{
	int r;
	if (!use_seize)
		return ptrace(PTRACE_ATTACH, pid, 0, 0);
	r = ptrace(PTRACE_SEIZE, pid, 0, PTRACE_SEIZE_DEVEL);
	if (r)
		return r;
	r = ptrace(PTRACE_INTERRUPT, pid, 0, 0);
	return r;
}
#else
# define ptrace_attach_or_seize(pid) ptrace(PTRACE_ATTACH, (pid), 0, 0)
#endif

static void
set_cloexec_flag(int fd)
{
	int flags, newflags;

	flags = fcntl(fd, F_GETFD);
	if (flags < 0) {
		/* Can happen only if fd is bad.
		 * Should never happen: if it does, we have a bug
		 * in the caller. Therefore we just abort
		 * instead of propagating the error.
		 */
		perror_msg_and_die("fcntl(%d, F_GETFD)", fd);
	}

	newflags = flags | FD_CLOEXEC;
	if (flags == newflags)
		return;

	fcntl(fd, F_SETFD, newflags); /* never fails */
}

/*
 * When strace is setuid executable, we have to swap uids
 * before and after filesystem and process management operations.
 */
static void
swap_uid(void)
{
#ifndef SVR4
	int euid = geteuid(), uid = getuid();

	if (euid != uid && setreuid(euid, uid) < 0) {
		perror_msg_and_die("setreuid");
	}
#endif
}

#if _LFS64_LARGEFILE
# define fopen_for_output fopen64
#else
# define fopen_for_output fopen
#endif

static FILE *
strace_fopen(const char *path)
{
	FILE *fp;

	swap_uid();
	fp = fopen_for_output(path, "w");
	if (!fp)
		perror_msg_and_die("Can't fopen '%s'", path);
	swap_uid();
	set_cloexec_flag(fileno(fp));
	return fp;
}

static int popen_pid = 0;

#ifndef _PATH_BSHELL
# define _PATH_BSHELL "/bin/sh"
#endif

/*
 * We cannot use standard popen(3) here because we have to distinguish
 * popen child process from other processes we trace, and standard popen(3)
 * does not export its child's pid.
 */
static FILE *
strace_popen(const char *command)
{
	FILE *fp;
	int fds[2];

	swap_uid();
	if (pipe(fds) < 0)
		perror_msg_and_die("pipe");

	set_cloexec_flag(fds[1]); /* never fails */

	popen_pid = vfork();
	if (popen_pid == -1)
		perror_msg_and_die("vfork");

	if (popen_pid == 0) {
		/* child */
		close(fds[1]);
		if (fds[0] != 0) {
			if (dup2(fds[0], 0))
				perror_msg_and_die("dup2");
			close(fds[0]);
		}
		execl(_PATH_BSHELL, "sh", "-c", command, NULL);
		perror_msg_and_die("Can't execute '%s'", _PATH_BSHELL);
	}

	/* parent */
	close(fds[0]);
	swap_uid();
	fp = fdopen(fds[1], "w");
	if (!fp)
		die_out_of_memory();
	return fp;
}

static void
newoutf(struct tcb *tcp)
{
	if (outfname && followfork > 1) {
		char name[520 + sizeof(int) * 3];
		sprintf(name, "%.512s.%u", outfname, tcp->pid);
		tcp->outf = strace_fopen(name);
	}
}

static void
startup_attach(void)
{
	int tcbi;
	struct tcb *tcp;

	/*
	 * Block user interruptions as we would leave the traced
	 * process stopped (process state T) if we would terminate in
	 * between PTRACE_ATTACH and wait4() on SIGSTOP.
	 * We rely on cleanup() from this point on.
	 */
	if (interactive)
		sigprocmask(SIG_BLOCK, &blocked_set, NULL);

	if (daemonized_tracer) {
		pid_t pid = fork();
		if (pid < 0) {
			perror_msg_and_die("fork");
		}
		if (pid) { /* parent */
			/*
			 * Wait for grandchild to attach to straced process
			 * (grandparent). Grandchild SIGKILLs us after it attached.
			 * Grandparent's wait() is unblocked by our death,
			 * it proceeds to exec the straced program.
			 */
			pause();
			_exit(0); /* paranoia */
		}
		/* grandchild */
		/* We will be the tracer process. Remember our new pid: */
		strace_tracer_pid = getpid();
	}

	for (tcbi = 0; tcbi < tcbtabsize; tcbi++) {
		tcp = tcbtab[tcbi];

		/* Is this a process we should attach to, but not yet attached? */
		if ((tcp->flags & (TCB_ATTACHED | TCB_STARTUP)) != TCB_ATTACHED)
			continue; /* no */

		/* Reinitialize the output since it may have changed */
		tcp->outf = outf;
		newoutf(tcp);

#ifdef USE_PROCFS
		if (proc_open(tcp, 1) < 0) {
			fprintf(stderr, "trouble opening proc file\n");
			droptcb(tcp);
			continue;
		}
#else /* !USE_PROCFS */
# ifdef LINUX
		if (followfork && !daemonized_tracer) {
			char procdir[sizeof("/proc/%d/task") + sizeof(int) * 3];
			DIR *dir;

			sprintf(procdir, "/proc/%d/task", tcp->pid);
			dir = opendir(procdir);
			if (dir != NULL) {
				unsigned int ntid = 0, nerr = 0;
				struct dirent *de;

				while ((de = readdir(dir)) != NULL) {
					struct tcb *cur_tcp;
					int tid;

					if (de->d_fileno == 0)
						continue;
					tid = atoi(de->d_name);
					if (tid <= 0)
						continue;
					++ntid;
					if (ptrace_attach_or_seize(tid) < 0) {
						++nerr;
						if (debug)
							fprintf(stderr, "attach to pid %d failed\n", tid);
						continue;
					}
					if (debug)
						fprintf(stderr, "attach to pid %d succeeded\n", tid);
					cur_tcp = tcp;
					if (tid != tcp->pid)
						cur_tcp = alloctcb(tid);
					cur_tcp->flags |= TCB_ATTACHED | TCB_STARTUP | post_attach_sigstop;
				}
				closedir(dir);
				if (interactive) {
					sigprocmask(SIG_SETMASK, &empty_set, NULL);
					if (interrupted)
						goto ret;
					sigprocmask(SIG_BLOCK, &blocked_set, NULL);
				}
				ntid -= nerr;
				if (ntid == 0) {
					perror("attach: ptrace(PTRACE_ATTACH, ...)");
					droptcb(tcp);
					continue;
				}
				if (!qflag) {
					fprintf(stderr, ntid > 1
? "Process %u attached with %u threads - interrupt to quit\n"
: "Process %u attached - interrupt to quit\n",
						tcp->pid, ntid);
				}
				if (!(tcp->flags & TCB_STARTUP)) {
					/* -p PID, we failed to attach to PID itself
					 * but did attach to some of its sibling threads.
					 * Drop PID's tcp.
					 */
					droptcb(tcp);
				}
				continue;
			} /* if (opendir worked) */
		} /* if (-f) */
# endif /* LINUX */
		if (ptrace_attach_or_seize(tcp->pid) < 0) {
			perror("attach: ptrace(PTRACE_ATTACH, ...)");
			droptcb(tcp);
			continue;
		}
		tcp->flags |= TCB_STARTUP | post_attach_sigstop;
		if (debug)
			fprintf(stderr, "attach to pid %d (main) succeeded\n", tcp->pid);

		if (daemonized_tracer) {
			/*
			 * It is our grandparent we trace, not a -p PID.
			 * Don't want to just detach on exit, so...
			 */
			tcp->flags &= ~TCB_ATTACHED;
			/*
			 * Make parent go away.
			 * Also makes grandparent's wait() unblock.
			 */
			kill(getppid(), SIGKILL);
		}

#endif /* !USE_PROCFS */
		if (!qflag)
			fprintf(stderr,
				"Process %u attached - interrupt to quit\n",
				tcp->pid);
	} /* for each tcbtab[] */

 ret:
	if (interactive)
		sigprocmask(SIG_SETMASK, &empty_set, NULL);
}

static void
startup_child(char **argv)
{
	struct stat statbuf;
	const char *filename;
	char pathname[MAXPATHLEN];
	int pid = 0;
	struct tcb *tcp;

	filename = argv[0];
	if (strchr(filename, '/')) {
		if (strlen(filename) > sizeof pathname - 1) {
			errno = ENAMETOOLONG;
			perror_msg_and_die("exec");
		}
		strcpy(pathname, filename);
	}
#ifdef USE_DEBUGGING_EXEC
	/*
	 * Debuggers customarily check the current directory
	 * first regardless of the path but doing that gives
	 * security geeks a panic attack.
	 */
	else if (stat(filename, &statbuf) == 0)
		strcpy(pathname, filename);
#endif /* USE_DEBUGGING_EXEC */
	else {
		const char *path;
		int m, n, len;

		for (path = getenv("PATH"); path && *path; path += m) {
			const char *colon = strchr(path, ':');
			if (colon) {
				n = colon - path;
				m = n + 1;
			}
			else
				m = n = strlen(path);
			if (n == 0) {
				if (!getcwd(pathname, MAXPATHLEN))
					continue;
				len = strlen(pathname);
			}
			else if (n > sizeof pathname - 1)
				continue;
			else {
				strncpy(pathname, path, n);
				len = n;
			}
			if (len && pathname[len - 1] != '/')
				pathname[len++] = '/';
			strcpy(pathname + len, filename);
			if (stat(pathname, &statbuf) == 0 &&
			    /* Accept only regular files
			       with some execute bits set.
			       XXX not perfect, might still fail */
			    S_ISREG(statbuf.st_mode) &&
			    (statbuf.st_mode & 0111))
				break;
		}
	}
	if (stat(pathname, &statbuf) < 0) {
		perror_msg_and_die("Can't stat '%s'", filename);
	}
	strace_child = pid = fork();
	if (pid < 0) {
		perror_msg_and_die("fork");
	}
	if ((pid != 0 && daemonized_tracer) /* -D: parent to become a traced process */
	 || (pid == 0 && !daemonized_tracer) /* not -D: child to become a traced process */
	) {
		pid = getpid();
		if (outf != stderr)
			close(fileno(outf));
#ifdef USE_PROCFS
# ifdef MIPS
		/* Kludge for SGI, see proc_open for details. */
		sa.sa_handler = foobar;
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
# endif
# ifndef FREEBSD
		pause();
# else
		kill(pid, SIGSTOP);
# endif
#else /* !USE_PROCFS */
		if (!daemonized_tracer && !use_seize) {
			if (ptrace(PTRACE_TRACEME, 0L, 0L, 0L) < 0) {
				perror_msg_and_die("ptrace(PTRACE_TRACEME, ...)");
			}
		}

		if (username != NULL) {
			uid_t run_euid = run_uid;
			gid_t run_egid = run_gid;

			if (statbuf.st_mode & S_ISUID)
				run_euid = statbuf.st_uid;
			if (statbuf.st_mode & S_ISGID)
				run_egid = statbuf.st_gid;
			/*
			 * It is important to set groups before we
			 * lose privileges on setuid.
			 */
			if (initgroups(username, run_gid) < 0) {
				perror_msg_and_die("initgroups");
			}
			if (setregid(run_gid, run_egid) < 0) {
				perror_msg_and_die("setregid");
			}
			if (setreuid(run_uid, run_euid) < 0) {
				perror_msg_and_die("setreuid");
			}
		}
		else if (geteuid() != 0)
			setreuid(run_uid, run_uid);

		if (!daemonized_tracer) {
			/*
			 * Induce a ptrace stop. Tracer (our parent)
			 * will resume us with PTRACE_SYSCALL and display
			 * the immediately following execve syscall.
			 * Can't do this on NOMMU systems, we are after
			 * vfork: parent is blocked, stopping would deadlock.
			 */
			if (!strace_vforked)
				kill(pid, SIGSTOP);
		} else {
			struct sigaction sv_sigchld;
			sigaction(SIGCHLD, NULL, &sv_sigchld);
			/*
			 * Make sure it is not SIG_IGN, otherwise wait
			 * will not block.
			 */
			signal(SIGCHLD, SIG_DFL);
			/*
			 * Wait for grandchild to attach to us.
			 * It kills child after that, and wait() unblocks.
			 */
			alarm(3);
			wait(NULL);
			alarm(0);
			sigaction(SIGCHLD, &sv_sigchld, NULL);
		}
#endif /* !USE_PROCFS */

		execv(pathname, argv);
		perror_msg_and_die("exec");
	}

	/* We are the tracer */

	if (!daemonized_tracer) {
		if (!use_seize) {
			/* child did PTRACE_TRACEME, nothing to do in parent */
		} else {
			if (!strace_vforked) {
				/* Wait until child stopped itself */
				int status;
				while (waitpid(pid, &status, WSTOPPED) < 0) {
					if (errno == EINTR)
						continue;
					perror_msg_and_die("waitpid");
				}
				if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
					kill(pid, SIGKILL);
					perror_msg_and_die("Unexpected wait status %x", status);
				}
			}
			/* Else: vforked case, we have no way to sync.
			 * Just attach to it as soon as possible.
			 * This means that we may miss a few first syscalls...
			 */

			if (ptrace_attach_or_seize(pid)) {
				kill(pid, SIGKILL);
				perror_msg_and_die("Can't attach to %d", pid);
			}
			if (!strace_vforked)
				kill(pid, SIGCONT);
		}
		tcp = alloctcb(pid);
		if (!strace_vforked)
			tcp->flags |= TCB_STARTUP | post_attach_sigstop;
		else
			tcp->flags |= TCB_STARTUP;
	}
	else {
		/* With -D, *we* are child here, IOW: different pid. Fetch it: */
		strace_tracer_pid = getpid();
		/* The tracee is our parent: */
		pid = getppid();
		tcp = alloctcb(pid);
		/* We want subsequent startup_attach() to attach to it: */
		tcp->flags |= TCB_ATTACHED;
	}
#ifdef USE_PROCFS
	if (proc_open(tcp, 0) < 0) {
		perror_msg_and_die("trouble opening proc file");
	}
#endif
}

#ifdef LINUX
static void kill_save_errno(pid_t pid, int sig)
{
	int saved_errno = errno;

	(void) kill(pid, sig);
	errno = saved_errno;
}

/*
 * Test whether the kernel support PTRACE_O_TRACECLONE et al options.
 * First fork a new child, call ptrace with PTRACE_SETOPTIONS on it,
 * and then see which options are supported by the kernel.
 */
static void
test_ptrace_setoptions_followfork(void)
{
	int pid, expected_grandchild = 0, found_grandchild = 0;
	const unsigned int test_options = PTRACE_O_TRACECLONE |
					  PTRACE_O_TRACEFORK |
					  PTRACE_O_TRACEVFORK;

	pid = fork();
	if (pid < 0)
		perror_msg_and_die("fork");
	if (pid == 0) {
		pid = getpid();
		if (ptrace(PTRACE_TRACEME, 0L, 0L, 0L) < 0)
			perror_msg_and_die("%s: PTRACE_TRACEME doesn't work",
					   __func__);
		kill(pid, SIGSTOP);
		if (fork() < 0)
			perror_msg_and_die("fork");
		_exit(0);
	}

	while (1) {
		int status, tracee_pid;

		errno = 0;
		tracee_pid = wait(&status);
		if (tracee_pid <= 0) {
			if (errno == EINTR)
				continue;
			else if (errno == ECHILD)
				break;
			kill_save_errno(pid, SIGKILL);
			perror_msg_and_die("%s: unexpected wait result %d",
					   __func__, tracee_pid);
		}
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status)) {
				if (tracee_pid != pid)
					kill_save_errno(pid, SIGKILL);
				error_msg_and_die("%s: unexpected exit status %u",
						  __func__, WEXITSTATUS(status));
			}
			continue;
		}
		if (WIFSIGNALED(status)) {
			if (tracee_pid != pid)
				kill_save_errno(pid, SIGKILL);
			error_msg_and_die("%s: unexpected signal %u",
					  __func__, WTERMSIG(status));
		}
		if (!WIFSTOPPED(status)) {
			if (tracee_pid != pid)
				kill_save_errno(tracee_pid, SIGKILL);
			kill(pid, SIGKILL);
			error_msg_and_die("%s: unexpected wait status %x",
					  __func__, status);
		}
		if (tracee_pid != pid) {
			found_grandchild = tracee_pid;
			if (ptrace(PTRACE_CONT, tracee_pid, 0, 0) < 0) {
				kill_save_errno(tracee_pid, SIGKILL);
				kill_save_errno(pid, SIGKILL);
				perror_msg_and_die("PTRACE_CONT doesn't work");
			}
			continue;
		}
		switch (WSTOPSIG(status)) {
		case SIGSTOP:
			if (ptrace(PTRACE_SETOPTIONS, pid, 0, test_options) < 0
			    && errno != EINVAL && errno != EIO)
				perror_msg("PTRACE_SETOPTIONS");
			break;
		case SIGTRAP:
			if (status >> 16 == PTRACE_EVENT_FORK) {
				long msg = 0;

				if (ptrace(PTRACE_GETEVENTMSG, pid,
					   NULL, (long) &msg) == 0)
					expected_grandchild = msg;
			}
			break;
		}
		if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0) {
			kill_save_errno(pid, SIGKILL);
			perror_msg_and_die("PTRACE_SYSCALL doesn't work");
		}
	}
	if (expected_grandchild && expected_grandchild == found_grandchild) {
		ptrace_setoptions |= test_options;
		if (debug)
			fprintf(stderr, "ptrace_setoptions = %#x\n",
				ptrace_setoptions);
		return;
	}
	error_msg("Test for PTRACE_O_TRACECLONE failed, "
		  "giving up using this feature.");
}

/*
 * Test whether the kernel support PTRACE_O_TRACESYSGOOD.
 * First fork a new child, call ptrace(PTRACE_SETOPTIONS) on it,
 * and then see whether it will stop with (SIGTRAP | 0x80).
 *
 * Use of this option enables correct handling of user-generated SIGTRAPs,
 * and SIGTRAPs generated by special instructions such as int3 on x86:
 * _start:	.globl	_start
 *		int3
 *		movl	$42, %ebx
 *		movl	$1, %eax
 *		int	$0x80
 * (compile with: "gcc -nostartfiles -nostdlib -o int3 int3.S")
 */
static void
test_ptrace_setoptions_for_all(void)
{
	const unsigned int test_options = PTRACE_O_TRACESYSGOOD |
					  PTRACE_O_TRACEEXEC;
	int pid;
	int it_worked = 0;

	pid = fork();
	if (pid < 0)
		perror_msg_and_die("fork");

	if (pid == 0) {
		pid = getpid();
		if (ptrace(PTRACE_TRACEME, 0L, 0L, 0L) < 0)
			/* Note: exits with exitcode 1 */
			perror_msg_and_die("%s: PTRACE_TRACEME doesn't work",
					   __func__);
		kill(pid, SIGSTOP);
		_exit(0); /* parent should see entry into this syscall */
	}

	while (1) {
		int status, tracee_pid;

		errno = 0;
		tracee_pid = wait(&status);
		if (tracee_pid <= 0) {
			if (errno == EINTR)
				continue;
			kill_save_errno(pid, SIGKILL);
			perror_msg_and_die("%s: unexpected wait result %d",
					   __func__, tracee_pid);
		}
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) == 0)
				break;
			error_msg_and_die("%s: unexpected exit status %u",
					  __func__, WEXITSTATUS(status));
		}
		if (WIFSIGNALED(status)) {
			error_msg_and_die("%s: unexpected signal %u",
					  __func__, WTERMSIG(status));
		}
		if (!WIFSTOPPED(status)) {
			kill(pid, SIGKILL);
			error_msg_and_die("%s: unexpected wait status %x",
					  __func__, status);
		}
		if (WSTOPSIG(status) == SIGSTOP) {
			/*
			 * We don't check "options aren't accepted" error.
			 * If it happens, we'll never get (SIGTRAP | 0x80),
			 * and thus will decide to not use the option.
			 * IOW: the outcome of the test will be correct.
			 */
			if (ptrace(PTRACE_SETOPTIONS, pid, 0L, test_options) < 0
			    && errno != EINVAL && errno != EIO)
				perror_msg("PTRACE_SETOPTIONS");
		}
		if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {
			it_worked = 1;
		}
		if (ptrace(PTRACE_SYSCALL, pid, 0L, 0L) < 0) {
			kill_save_errno(pid, SIGKILL);
			perror_msg_and_die("PTRACE_SYSCALL doesn't work");
		}
	}

	if (it_worked) {
		syscall_trap_sig = (SIGTRAP | 0x80);
		ptrace_setoptions |= test_options;
		if (debug)
			fprintf(stderr, "ptrace_setoptions = %#x\n",
				ptrace_setoptions);
		return;
	}

	error_msg("Test for PTRACE_O_TRACESYSGOOD failed, "
		  "giving up using this feature.");
}

# ifdef USE_SEIZE
static void
test_ptrace_seize(void)
{
	int pid;

	pid = fork();
	if (pid < 0)
		perror_msg_and_die("fork");

	if (pid == 0) {
		pause();
		_exit(0);
	}

	/* PTRACE_SEIZE, unlike ATTACH, doesn't force tracee to trap.  After
	 * attaching tracee continues to run unless a trap condition occurs.
	 * PTRACE_SEIZE doesn't affect signal or group stop state.
	 */
	if (ptrace(PTRACE_SEIZE, pid, 0, PTRACE_SEIZE_DEVEL) == 0) {
		post_attach_sigstop = 0; /* this sets use_seize to 1 */
	} else if (debug) {
		fprintf(stderr, "PTRACE_SEIZE doesn't work\n");
	}

	kill(pid, SIGKILL);

	while (1) {
		int status, tracee_pid;

		errno = 0;
		tracee_pid = waitpid(pid, &status, 0);
		if (tracee_pid <= 0) {
			if (errno == EINTR)
				continue;
			perror_msg_and_die("%s: unexpected wait result %d",
					 __func__, tracee_pid);
		}
		if (WIFSIGNALED(status)) {
			return;
		}
		error_msg_and_die("%s: unexpected wait status %x",
				__func__, status);
	}
}
# else /* !USE_SEIZE */
#  define test_ptrace_seize() ((void)0)
# endif

#endif

/* Noinline: don't want main to have struct utsname permanently on stack */
static void __attribute__ ((noinline))
get_os_release(void)
{
	struct utsname u;
	if (uname(&u) < 0)
		perror_msg_and_die("uname");
	os_release = strdup(u.release);
	if (!os_release)
		die_out_of_memory();
}

int
main(int argc, char *argv[])
{
	struct tcb *tcp;
	int c, pid = 0;
	int optF = 0;
	struct sigaction sa;

	progname = argv[0] ? argv[0] : "strace";

	strace_tracer_pid = getpid();

	get_os_release();

	/* Allocate the initial tcbtab.  */
	tcbtabsize = argc;	/* Surely enough for all -p args.  */
	tcbtab = calloc(tcbtabsize, sizeof(tcbtab[0]));
	if (!tcbtab)
		die_out_of_memory();
	tcp = calloc(tcbtabsize, sizeof(*tcp));
	if (!tcp)
		die_out_of_memory();
	for (c = 0; c < tcbtabsize; c++)
		tcbtab[c] = tcp++;

	outf = stderr;
	set_sortby(DEFAULT_SORTBY);
	set_personality(DEFAULT_PERSONALITY);
	qualify("trace=all");
	qualify("abbrev=all");
	qualify("verbose=all");
	qualify("signal=all");
	while ((c = getopt(argc, argv,
		"+cCdfFhiqrtTvVxyz"
#ifndef USE_PROCFS
		"D"
#endif
		"a:e:o:O:p:s:S:u:E:P:I:")) != EOF) {
		switch (c) {
		case 'c':
			if (cflag == CFLAG_BOTH) {
				error_msg_and_die("-c and -C are mutually exclusive options");
			}
			cflag = CFLAG_ONLY_STATS;
			break;
		case 'C':
			if (cflag == CFLAG_ONLY_STATS) {
				error_msg_and_die("-c and -C are mutually exclusive options");
			}
			cflag = CFLAG_BOTH;
			break;
		case 'd':
			debug++;
			break;
#ifndef USE_PROCFS
		case 'D':
			daemonized_tracer = 1;
			break;
#endif
		case 'F':
			optF = 1;
			break;
		case 'f':
			followfork++;
			break;
		case 'h':
			usage(stdout, 0);
			break;
		case 'i':
			iflag++;
			break;
		case 'q':
			qflag++;
			break;
		case 'r':
			rflag++;
			tflag++;
			break;
		case 't':
			tflag++;
			break;
		case 'T':
			dtime++;
			break;
		case 'x':
			xflag++;
			break;
		case 'y':
			show_fd_path = 1;
			break;
		case 'v':
			qualify("abbrev=none");
			break;
		case 'V':
			printf("%s -- version %s\n", PACKAGE_NAME, VERSION);
			exit(0);
			break;
		case 'z':
			not_failing_only = 1;
			break;
		case 'a':
			acolumn = atoi(optarg);
			if (acolumn < 0)
				error_msg_and_die("Bad column width '%s'", optarg);
			break;
		case 'e':
			qualify(optarg);
			break;
		case 'o':
			outfname = strdup(optarg);
			break;
		case 'O':
			set_overhead(atoi(optarg));
			break;
		case 'p':
			pid = atoi(optarg);
			if (pid <= 0) {
				error_msg("Invalid process id: '%s'", optarg);
				break;
			}
			if (pid == strace_tracer_pid) {
				error_msg("I'm sorry, I can't let you do that, Dave.");
				break;
			}
			tcp = alloc_tcb(pid, 0);
			tcp->flags |= TCB_ATTACHED;
			pflag_seen++;
			break;
		case 'P':
			tracing_paths = 1;
			if (pathtrace_select(optarg)) {
				error_msg_and_die("Failed to select path '%s'", optarg);
			}
			break;
		case 's':
			max_strlen = atoi(optarg);
			if (max_strlen < 0) {
				error_msg_and_die("Invalid -%c argument: '%s'", c, optarg);
			}
			break;
		case 'S':
			set_sortby(optarg);
			break;
		case 'u':
			username = strdup(optarg);
			break;
		case 'E':
			if (putenv(optarg) < 0)
				die_out_of_memory();
			break;
		case 'I':
			opt_intr = atoi(optarg);
			if (opt_intr <= 0 || opt_intr >= NUM_INTR_OPTS) {
				error_msg_and_die("Invalid -%c argument: '%s'", c, optarg);
			}
			break;
		default:
			usage(stderr, 1);
			break;
		}
	}
	argv += optind;
	/* argc -= optind; - no need, argc is not used below */

	acolumn_spaces = malloc(acolumn + 1);
	if (!acolumn_spaces)
		die_out_of_memory();
	memset(acolumn_spaces, ' ', acolumn);
	acolumn_spaces[acolumn] = '\0';

	/* Must have PROG [ARGS], or -p PID. Not both. */
	if (!argv[0] == !pflag_seen)
		usage(stderr, 1);

	if (pflag_seen && daemonized_tracer) {
		error_msg_and_die("-D and -p are mutually exclusive options");
	}

	if (!followfork)
		followfork = optF;

	if (followfork > 1 && cflag) {
		error_msg_and_die("(-c or -C) and -ff are mutually exclusive options");
	}

	/* See if they want to run as another user. */
	if (username != NULL) {
		struct passwd *pent;

		if (getuid() != 0 || geteuid() != 0) {
			error_msg_and_die("You must be root to use the -u option");
		}
		pent = getpwnam(username);
		if (pent == NULL) {
			error_msg_and_die("Cannot find user '%s'", username);
		}
		run_uid = pent->pw_uid;
		run_gid = pent->pw_gid;
	}
	else {
		run_uid = getuid();
		run_gid = getgid();
	}

#ifdef LINUX
	if (followfork)
		test_ptrace_setoptions_followfork();
	test_ptrace_setoptions_for_all();
	test_ptrace_seize();
#endif

	/* Check if they want to redirect the output. */
	if (outfname) {
		/* See if they want to pipe the output. */
		if (outfname[0] == '|' || outfname[0] == '!') {
			/*
			 * We can't do the <outfname>.PID funny business
			 * when using popen, so prohibit it.
			 */
			if (followfork > 1)
				error_msg_and_die("Piping the output and -ff are mutually exclusive");
			outf = strace_popen(outfname + 1);
		}
		else if (followfork <= 1)
			outf = strace_fopen(outfname);
	}

	if (!outfname || outfname[0] == '|' || outfname[0] == '!') {
		char *buf = malloc(BUFSIZ);
		if (!buf)
			die_out_of_memory();
		setvbuf(outf, buf, _IOLBF, BUFSIZ);
	}
	if (outfname && argv[0]) {
		if (!opt_intr)
			opt_intr = INTR_NEVER;
		qflag = 1;
	}
	if (!opt_intr)
		opt_intr = INTR_WHILE_WAIT;

	/* argv[0]	-pPID	-oFILE	Default interactive setting
	 * yes		0	0	INTR_WHILE_WAIT
	 * no		1	0	INTR_WHILE_WAIT
	 * yes		0	1	INTR_NEVER
	 * no		1	1	INTR_WHILE_WAIT
	 */

	/* STARTUP_CHILD must be called before the signal handlers get
	   installed below as they are inherited into the spawned process.
	   Also we do not need to be protected by them as during interruption
	   in the STARTUP_CHILD mode we kill the spawned process anyway.  */
	if (argv[0])
		startup_child(argv);

	sigemptyset(&empty_set);
	sigemptyset(&blocked_set);
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTTOU, &sa, NULL); /* SIG_IGN */
	sigaction(SIGTTIN, &sa, NULL); /* SIG_IGN */
	if (opt_intr != INTR_ANYWHERE) {
		if (opt_intr == INTR_BLOCK_TSTP_TOO)
			sigaction(SIGTSTP, &sa, NULL); /* SIG_IGN */
		/*
		 * In interactive mode (if no -o OUTFILE, or -p PID is used),
		 * fatal signals are blocked while syscall stop is processed,
		 * and acted on in between, when waiting for new syscall stops.
		 * In non-interactive mode, signals are ignored.
		 */
		if (opt_intr == INTR_WHILE_WAIT) {
			sigaddset(&blocked_set, SIGHUP);
			sigaddset(&blocked_set, SIGINT);
			sigaddset(&blocked_set, SIGQUIT);
			sigaddset(&blocked_set, SIGPIPE);
			sigaddset(&blocked_set, SIGTERM);
			sa.sa_handler = interrupt;
#ifdef SUNOS4
			/* POSIX signals on sunos4.1 are a little broken. */
			sa.sa_flags = SA_INTERRUPT;
#endif
		}
		/* SIG_IGN, or set handler for these */
		sigaction(SIGHUP, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGPIPE, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}
#ifdef USE_PROCFS
	sa.sa_handler = reaper;
	sigaction(SIGCHLD, &sa, NULL);
#else
	/* Make sure SIGCHLD has the default action so that waitpid
	   definitely works without losing track of children.  The user
	   should not have given us a bogus state to inherit, but he might
	   have.  Arguably we should detect SIG_IGN here and pass it on
	   to children, but probably noone really needs that.  */
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
#endif /* USE_PROCFS */

	if (pflag_seen || daemonized_tracer)
		startup_attach();

	if (trace() < 0)
		exit(1);

	cleanup();
	fflush(NULL);
	if (exit_code > 0xff) {
		/* Child was killed by a signal, mimic that.  */
		exit_code &= 0xff;
		signal(exit_code, SIG_DFL);
		raise(exit_code);
		/* Paranoia - what if this signal is not fatal?
		   Exit with 128 + signo then.  */
		exit_code += 128;
	}
	exit(exit_code);
}

static void
expand_tcbtab(void)
{
	/* Allocate some more TCBs and expand the table.
	   We don't want to relocate the TCBs because our
	   callers have pointers and it would be a pain.
	   So tcbtab is a table of pointers.  Since we never
	   free the TCBs, we allocate a single chunk of many.  */
	int i = tcbtabsize;
	struct tcb *newtcbs = calloc(tcbtabsize, sizeof(newtcbs[0]));
	struct tcb **newtab = realloc(tcbtab, tcbtabsize * 2 * sizeof(tcbtab[0]));
	if (!newtab || !newtcbs)
		die_out_of_memory();
	tcbtabsize *= 2;
	tcbtab = newtab;
	while (i < tcbtabsize)
		tcbtab[i++] = newtcbs++;
}

struct tcb *
alloc_tcb(int pid, int command_options_parsed)
{
	int i;
	struct tcb *tcp;

	if (nprocs == tcbtabsize)
		expand_tcbtab();

	for (i = 0; i < tcbtabsize; i++) {
		tcp = tcbtab[i];
		if ((tcp->flags & TCB_INUSE) == 0) {
			memset(tcp, 0, sizeof(*tcp));
			tcp->pid = pid;
			tcp->flags = TCB_INUSE;
			tcp->outf = outf; /* Initialise to current out file */
#if SUPPORTED_PERSONALITIES > 1
			tcp->currpers = current_personality;
#endif
#ifdef USE_PROCFS
			tcp->pfd = -1;
#endif
			nprocs++;
			if (debug)
				fprintf(stderr, "new tcb for pid %d, active tcbs:%d\n", tcp->pid, nprocs);
			if (command_options_parsed)
				newoutf(tcp);
			return tcp;
		}
	}
	error_msg_and_die("bug in alloc_tcb");
}

#ifdef USE_PROCFS
int
proc_open(struct tcb *tcp, int attaching)
{
	char proc[32];
	long arg;
#ifdef SVR4
	int i;
	sysset_t syscalls;
	sigset_t signals;
	fltset_t faults;
#endif
#ifndef HAVE_POLLABLE_PROCFS
	static int last_pfd;
#endif

#ifdef HAVE_MP_PROCFS
	/* Open the process pseudo-files in /proc. */
	sprintf(proc, "/proc/%d/ctl", tcp->pid);
	tcp->pfd = open(proc, O_WRONLY|O_EXCL);
	if (tcp->pfd < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	set_cloexec_flag(tcp->pfd);
	sprintf(proc, "/proc/%d/status", tcp->pid);
	tcp->pfd_stat = open(proc, O_RDONLY|O_EXCL);
	if (tcp->pfd_stat < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	set_cloexec_flag(tcp->pfd_stat);
	sprintf(proc, "/proc/%d/as", tcp->pid);
	tcp->pfd_as = open(proc, O_RDONLY|O_EXCL);
	if (tcp->pfd_as < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	set_cloexec_flag(tcp->pfd_as);
#else
	/* Open the process pseudo-file in /proc. */
# ifndef FREEBSD
	sprintf(proc, "/proc/%d", tcp->pid);
	tcp->pfd = open(proc, O_RDWR|O_EXCL);
# else
	sprintf(proc, "/proc/%d/mem", tcp->pid);
	tcp->pfd = open(proc, O_RDWR);
# endif
	if (tcp->pfd < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	set_cloexec_flag(tcp->pfd);
#endif
#ifdef FREEBSD
	sprintf(proc, "/proc/%d/regs", tcp->pid);
	tcp->pfd_reg = open(proc, O_RDONLY);
	if (tcp->pfd_reg < 0) {
		perror("strace: open(\"/proc/.../regs\", ...)");
		return -1;
	}
	if (cflag) {
		sprintf(proc, "/proc/%d/status", tcp->pid);
		tcp->pfd_status = open(proc, O_RDONLY);
		if (tcp->pfd_status < 0) {
			perror("strace: open(\"/proc/.../status\", ...)");
			return -1;
		}
	} else
		tcp->pfd_status = -1;
#endif /* FREEBSD */
	rebuild_pollv();
	if (!attaching) {
		/*
		 * Wait for the child to pause.  Because of a race
		 * condition we have to poll for the event.
		 */
		for (;;) {
			if (IOCTL_STATUS(tcp) < 0) {
				perror("strace: PIOCSTATUS");
				return -1;
			}
			if (tcp->status.PR_FLAGS & PR_ASLEEP)
				break;
		}
	}
#ifndef FREEBSD
	/* Stop the process so that we own the stop. */
	if (IOCTL(tcp->pfd, PIOCSTOP, (char *)NULL) < 0) {
		perror("strace: PIOCSTOP");
		return -1;
	}
#endif
#ifdef PIOCSET
	/* Set Run-on-Last-Close. */
	arg = PR_RLC;
	if (IOCTL(tcp->pfd, PIOCSET, &arg) < 0) {
		perror("PIOCSET PR_RLC");
		return -1;
	}
	/* Set or Reset Inherit-on-Fork. */
	arg = PR_FORK;
	if (IOCTL(tcp->pfd, followfork ? PIOCSET : PIOCRESET, &arg) < 0) {
		perror("PIOC{SET,RESET} PR_FORK");
		return -1;
	}
#else  /* !PIOCSET */
#ifndef FREEBSD
	if (ioctl(tcp->pfd, PIOCSRLC) < 0) {
		perror("PIOCSRLC");
		return -1;
	}
	if (ioctl(tcp->pfd, followfork ? PIOCSFORK : PIOCRFORK) < 0) {
		perror("PIOC{S,R}FORK");
		return -1;
	}
#else /* FREEBSD */
	/* just unset the PF_LINGER flag for the Run-on-Last-Close. */
	if (ioctl(tcp->pfd, PIOCGFL, &arg) < 0) {
	        perror("PIOCGFL");
		return -1;
	}
	arg &= ~PF_LINGER;
	if (ioctl(tcp->pfd, PIOCSFL, arg) < 0) {
		perror("PIOCSFL");
		return -1;
	}
#endif /* FREEBSD */
#endif /* !PIOCSET */
#ifndef FREEBSD
	/* Enable all syscall entries we care about. */
	premptyset(&syscalls);
	for (i = 1; i < MAX_QUALS; ++i) {
		if (i > (sizeof syscalls) * CHAR_BIT) break;
		if (qual_flags[i] & QUAL_TRACE) praddset(&syscalls, i);
	}
	praddset(&syscalls, SYS_execve);
	if (followfork) {
		praddset(&syscalls, SYS_fork);
#ifdef SYS_forkall
		praddset(&syscalls, SYS_forkall);
#endif
#ifdef SYS_fork1
		praddset(&syscalls, SYS_fork1);
#endif
#ifdef SYS_rfork1
		praddset(&syscalls, SYS_rfork1);
#endif
#ifdef SYS_rforkall
		praddset(&syscalls, SYS_rforkall);
#endif
	}
	if (IOCTL(tcp->pfd, PIOCSENTRY, &syscalls) < 0) {
		perror("PIOCSENTRY");
		return -1;
	}
	/* Enable the syscall exits. */
	if (IOCTL(tcp->pfd, PIOCSEXIT, &syscalls) < 0) {
		perror("PIOSEXIT");
		return -1;
	}
	/* Enable signals we care about. */
	premptyset(&signals);
	for (i = 1; i < MAX_QUALS; ++i) {
		if (i > (sizeof signals) * CHAR_BIT) break;
		if (qual_flags[i] & QUAL_SIGNAL) praddset(&signals, i);
	}
	if (IOCTL(tcp->pfd, PIOCSTRACE, &signals) < 0) {
		perror("PIOCSTRACE");
		return -1;
	}
	/* Enable faults we care about */
	premptyset(&faults);
	for (i = 1; i < MAX_QUALS; ++i) {
		if (i > (sizeof faults) * CHAR_BIT) break;
		if (qual_flags[i] & QUAL_FAULT) praddset(&faults, i);
	}
	if (IOCTL(tcp->pfd, PIOCSFAULT, &faults) < 0) {
		perror("PIOCSFAULT");
		return -1;
	}
#else /* FREEBSD */
	/* set events flags. */
	arg = S_SIG | S_SCE | S_SCX;
	if (ioctl(tcp->pfd, PIOCBIS, arg) < 0) {
		perror("PIOCBIS");
		return -1;
	}
#endif /* FREEBSD */
	if (!attaching) {
#ifdef MIPS
		/*
		 * The SGI PRSABORT doesn't work for pause() so
		 * we send it a caught signal to wake it up.
		 */
		kill(tcp->pid, SIGINT);
#else /* !MIPS */
#ifdef PRSABORT
		/* The child is in a pause(), abort it. */
		arg = PRSABORT;
		if (IOCTL(tcp->pfd, PIOCRUN, &arg) < 0) {
			perror("PIOCRUN");
			return -1;
		}
#endif
#endif /* !MIPS*/
#ifdef FREEBSD
		/* wake up the child if it received the SIGSTOP */
		kill(tcp->pid, SIGCONT);
#endif
		for (;;) {
			/* Wait for the child to do something. */
			if (IOCTL_WSTOP(tcp) < 0) {
				perror("PIOCWSTOP");
				return -1;
			}
			if (tcp->status.PR_WHY == PR_SYSENTRY) {
				tcp->flags &= ~TCB_INSYSCALL;
				get_scno(tcp);
				if (known_scno(tcp) == SYS_execve)
					break;
			}
			/* Set it running: maybe execve will be next. */
#ifndef FREEBSD
			arg = 0;
			if (IOCTL(tcp->pfd, PIOCRUN, &arg) < 0)
#else
			if (IOCTL(tcp->pfd, PIOCRUN, 0) < 0)
#endif
			{
				perror("PIOCRUN");
				return -1;
			}
#ifdef FREEBSD
			/* handle the case where we "opened" the child before
			   it did the kill -STOP */
			if (tcp->status.PR_WHY == PR_SIGNALLED &&
			    tcp->status.PR_WHAT == SIGSTOP)
			        kill(tcp->pid, SIGCONT);
#endif
		}
	}
#ifdef FREEBSD
	else {
		if (attaching < 2) {
			/* We are attaching to an already running process.
			 * Try to figure out the state of the process in syscalls,
			 * to handle the first event well.
			 * This is done by having a look at the "wchan" property of the
			 * process, which tells where it is stopped (if it is). */
			FILE * status;
			char wchan[20]; /* should be enough */

			sprintf(proc, "/proc/%d/status", tcp->pid);
			status = fopen(proc, "r");
			if (status &&
			    (fscanf(status, "%*s %*d %*d %*d %*d %*d,%*d %*s %*d,%*d"
				    "%*d,%*d %*d,%*d %19s", wchan) == 1) &&
			    strcmp(wchan, "nochan") && strcmp(wchan, "spread") &&
			    strcmp(wchan, "stopevent")) {
				/* The process is asleep in the middle of a syscall.
				   Fake the syscall entry event */
				tcp->flags &= ~(TCB_INSYSCALL|TCB_STARTUP);
				tcp->status.PR_WHY = PR_SYSENTRY;
				trace_syscall(tcp);
			}
			if (status)
				fclose(status);
		} /* otherwise it's a fork being followed */
	}
#endif /* FREEBSD */
#ifndef HAVE_POLLABLE_PROCFS
	if (proc_poll_pipe[0] != -1)
		proc_poller(tcp->pfd);
	else if (nprocs > 1) {
		proc_poll_open();
		proc_poller(last_pfd);
		proc_poller(tcp->pfd);
	}
	last_pfd = tcp->pfd;
#endif /* !HAVE_POLLABLE_PROCFS */
	return 0;
}

#endif /* USE_PROCFS */

static struct tcb *
pid2tcb(int pid)
{
	int i;

	if (pid <= 0)
		return NULL;

	for (i = 0; i < tcbtabsize; i++) {
		struct tcb *tcp = tcbtab[i];
		if (tcp->pid == pid && (tcp->flags & TCB_INUSE))
			return tcp;
	}

	return NULL;
}

#ifdef USE_PROCFS

static struct tcb *
first_used_tcb(void)
{
	int i;
	struct tcb *tcp;
	for (i = 0; i < tcbtabsize; i++) {
		tcp = tcbtab[i];
		if (tcp->flags & TCB_INUSE)
			return tcp;
	}
	return NULL;
}

static struct tcb *
pfd2tcb(int pfd)
{
	int i;

	for (i = 0; i < tcbtabsize; i++) {
		struct tcb *tcp = tcbtab[i];
		if (tcp->pfd != pfd)
			continue;
		if (tcp->flags & TCB_INUSE)
			return tcp;
	}
	return NULL;
}

#endif /* USE_PROCFS */

void
droptcb(struct tcb *tcp)
{
	if (tcp->pid == 0)
		return;

	nprocs--;
	if (debug)
		fprintf(stderr, "dropped tcb for pid %d, %d remain\n", tcp->pid, nprocs);

#ifdef USE_PROCFS
	if (tcp->pfd != -1) {
		close(tcp->pfd);
		tcp->pfd = -1;
# ifdef FREEBSD
		if (tcp->pfd_reg != -1) {
		        close(tcp->pfd_reg);
		        tcp->pfd_reg = -1;
		}
		if (tcp->pfd_status != -1) {
			close(tcp->pfd_status);
			tcp->pfd_status = -1;
		}
# endif
		tcp->flags = 0; /* rebuild_pollv needs it */
		rebuild_pollv();
	}
#endif

	if (outfname && followfork > 1 && tcp->outf)
		fclose(tcp->outf);

	memset(tcp, 0, sizeof(*tcp));
}

/* detach traced process; continue with sig
   Never call DETACH twice on the same process as both unattached and
   attached-unstopped processes give the same ESRCH.  For unattached process we
   would SIGSTOP it and wait for its SIGSTOP notification forever.  */

static int
detach(struct tcb *tcp)
{
	int error = 0;
#ifdef LINUX
	int status, catch_sigstop;
#endif

	if (tcp->flags & TCB_BPTSET)
		clearbpt(tcp);

#ifdef LINUX
	/*
	 * Linux wrongly insists the child be stopped
	 * before detaching.  Arghh.  We go through hoops
	 * to make a clean break of things.
	 */
#if defined(SPARC)
#undef PTRACE_DETACH
#define PTRACE_DETACH PTRACE_SUNDETACH
#endif
	/*
	 * We attached but possibly didn't see the expected SIGSTOP.
	 * We must catch exactly one as otherwise the detached process
	 * would be left stopped (process state T).
	 */
	catch_sigstop = (tcp->flags & TCB_IGNORE_ONE_SIGSTOP);
	error = ptrace(PTRACE_DETACH, tcp->pid, (char *) 1, 0);
	if (error == 0) {
		/* On a clear day, you can see forever. */
	}
	else if (errno != ESRCH) {
		/* Shouldn't happen. */
		perror("detach: ptrace(PTRACE_DETACH, ...)");
	}
	else if (my_tkill(tcp->pid, 0) < 0) {
		if (errno != ESRCH)
			perror("detach: checking sanity");
	}
	else if (!catch_sigstop && my_tkill(tcp->pid, SIGSTOP) < 0) {
		if (errno != ESRCH)
			perror("detach: stopping child");
	}
	else
		catch_sigstop = 1;
	if (catch_sigstop) {
		for (;;) {
#ifdef __WALL
			if (wait4(tcp->pid, &status, __WALL, NULL) < 0) {
				if (errno == ECHILD) /* Already gone.  */
					break;
				if (errno != EINVAL) {
					perror("detach: waiting");
					break;
				}
#endif /* __WALL */
				/* No __WALL here.  */
				if (waitpid(tcp->pid, &status, 0) < 0) {
					if (errno != ECHILD) {
						perror("detach: waiting");
						break;
					}
#ifdef __WCLONE
					/* If no processes, try clones.  */
					if (wait4(tcp->pid, &status, __WCLONE,
						  NULL) < 0) {
						if (errno != ECHILD)
							perror("detach: waiting");
						break;
					}
#endif /* __WCLONE */
				}
#ifdef __WALL
			}
#endif
			if (!WIFSTOPPED(status)) {
				/* Au revoir, mon ami. */
				break;
			}
			if (WSTOPSIG(status) == SIGSTOP) {
				ptrace_restart(PTRACE_DETACH, tcp, 0);
				break;
			}
			error = ptrace_restart(PTRACE_CONT, tcp,
					WSTOPSIG(status) == syscall_trap_sig ? 0
					: WSTOPSIG(status));
			if (error < 0)
				break;
		}
	}
#endif /* LINUX */

#if defined(SUNOS4)
	/* PTRACE_DETACH won't respect `sig' argument, so we post it here. */
	error = ptrace_restart(PTRACE_DETACH, tcp, 0);
#endif /* SUNOS4 */

	if (!qflag)
		fprintf(stderr, "Process %u detached\n", tcp->pid);

	droptcb(tcp);

	return error;
}

#ifdef USE_PROCFS

static void reaper(int sig)
{
	int pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
	}
}

#endif /* USE_PROCFS */

static void
cleanup(void)
{
	int i;
	struct tcb *tcp;
	int fatal_sig;

	/* 'interrupted' is a volatile object, fetch it only once */
	fatal_sig = interrupted;
	if (!fatal_sig)
		fatal_sig = SIGTERM;

	for (i = 0; i < tcbtabsize; i++) {
		tcp = tcbtab[i];
		if (!(tcp->flags & TCB_INUSE))
			continue;
		if (debug)
			fprintf(stderr,
				"cleanup: looking at pid %u\n", tcp->pid);
		if (printing_tcp &&
		    (!outfname || followfork < 2 || printing_tcp == tcp)) {
			tprints(" <unfinished ...>\n");
			printing_tcp = NULL;
		}
		if (tcp->flags & TCB_ATTACHED)
			detach(tcp);
		else {
			kill(tcp->pid, SIGCONT);
			kill(tcp->pid, fatal_sig);
		}
	}
	if (cflag)
		call_summary(outf);
}

static void
interrupt(int sig)
{
	interrupted = sig;
}

#ifndef HAVE_STRERROR

#if !HAVE_DECL_SYS_ERRLIST
extern int sys_nerr;
extern char *sys_errlist[];
#endif /* HAVE_DECL_SYS_ERRLIST */

const char *
strerror(int err_no)
{
	static char buf[64];

	if (err_no < 1 || err_no >= sys_nerr) {
		sprintf(buf, "Unknown error %d", err_no);
		return buf;
	}
	return sys_errlist[err_no];
}

#endif /* HAVE_STERRROR */

#ifndef HAVE_STRSIGNAL

#if defined HAVE_SYS_SIGLIST && !defined HAVE_DECL_SYS_SIGLIST
extern char *sys_siglist[];
#endif
#if defined HAVE_SYS__SIGLIST && !defined HAVE_DECL__SYS_SIGLIST
extern char *_sys_siglist[];
#endif

const char *
strsignal(int sig)
{
	static char buf[64];

	if (sig < 1 || sig >= NSIG) {
		sprintf(buf, "Unknown signal %d", sig);
		return buf;
	}
#ifdef HAVE__SYS_SIGLIST
	return _sys_siglist[sig];
#else
	return sys_siglist[sig];
#endif
}

#endif /* HAVE_STRSIGNAL */

#ifdef USE_PROCFS

static void
rebuild_pollv(void)
{
	int i, j;

	free(pollv);
	pollv = malloc(nprocs * sizeof(pollv[0]));
	if (!pollv)
		die_out_of_memory();

	for (i = j = 0; i < tcbtabsize; i++) {
		struct tcb *tcp = tcbtab[i];
		if (!(tcp->flags & TCB_INUSE))
			continue;
		pollv[j].fd = tcp->pfd;
		pollv[j].events = POLLWANT;
		j++;
	}
	if (j != nprocs) {
		error_msg_and_die("proc miscount");
	}
}

#ifndef HAVE_POLLABLE_PROCFS

static void
proc_poll_open(void)
{
	int i;

	if (pipe(proc_poll_pipe) < 0) {
		perror_msg_and_die("pipe");
	}
	for (i = 0; i < 2; i++) {
		set_cloexec_flag(proc_poll_pipe[i]);
	}
}

static int
proc_poll(struct pollfd *pollv, int nfds, int timeout)
{
	int i;
	int n;
	struct proc_pollfd pollinfo;

	n = read(proc_poll_pipe[0], &pollinfo, sizeof(pollinfo));
	if (n < 0)
		return n;
	if (n != sizeof(struct proc_pollfd)) {
		error_msg_and_die("panic: short read: %d", n);
	}
	for (i = 0; i < nprocs; i++) {
		if (pollv[i].fd == pollinfo.fd)
			pollv[i].revents = pollinfo.revents;
		else
			pollv[i].revents = 0;
	}
	poller_pid = pollinfo.pid;
	return 1;
}

static void
wakeup_handler(int sig)
{
}

static void
proc_poller(int pfd)
{
	struct proc_pollfd pollinfo;
	struct sigaction sa;
	sigset_t blocked_set, empty_set;
	int i;
	int n;
	struct rlimit rl;
#ifdef FREEBSD
	struct procfs_status pfs;
#endif /* FREEBSD */

	switch (fork()) {
	case -1:
		perror_msg_and_die("fork");
	case 0:
		break;
	default:
		return;
	}

	sa.sa_handler = interactive ? SIG_DFL : SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = wakeup_handler;
	sigaction(SIGUSR1, &sa, NULL);
	sigemptyset(&blocked_set);
	sigaddset(&blocked_set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &blocked_set, NULL);
	sigemptyset(&empty_set);

	if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
		perror_msg_and_die("getrlimit(RLIMIT_NOFILE, ...)");
	}
	n = rl.rlim_cur;
	for (i = 0; i < n; i++) {
		if (i != pfd && i != proc_poll_pipe[1])
			close(i);
	}

	pollinfo.fd = pfd;
	pollinfo.pid = getpid();
	for (;;) {
#ifndef FREEBSD
		if (ioctl(pfd, PIOCWSTOP, NULL) < 0)
#else
		if (ioctl(pfd, PIOCWSTOP, &pfs) < 0)
#endif
		{
			switch (errno) {
			case EINTR:
				continue;
			case EBADF:
				pollinfo.revents = POLLERR;
				break;
			case ENOENT:
				pollinfo.revents = POLLHUP;
				break;
			default:
				perror("proc_poller: PIOCWSTOP");
			}
			write(proc_poll_pipe[1], &pollinfo, sizeof(pollinfo));
			_exit(0);
		}
		pollinfo.revents = POLLWANT;
		write(proc_poll_pipe[1], &pollinfo, sizeof(pollinfo));
		sigsuspend(&empty_set);
	}
}

#endif /* !HAVE_POLLABLE_PROCFS */

static int
choose_pfd()
{
	int i, j;
	struct tcb *tcp;

	static int last;

	if (followfork < 2 &&
	    last < nprocs && (pollv[last].revents & POLLWANT)) {
		/*
		 * The previous process is ready to run again.  We'll
		 * let it do so if it is currently in a syscall.  This
		 * heuristic improves the readability of the trace.
		 */
		tcp = pfd2tcb(pollv[last].fd);
		if (tcp && exiting(tcp))
			return pollv[last].fd;
	}

	for (i = 0; i < nprocs; i++) {
		/* Let competing children run round robin. */
		j = (i + last + 1) % nprocs;
		if (pollv[j].revents & (POLLHUP | POLLERR)) {
			tcp = pfd2tcb(pollv[j].fd);
			if (!tcp) {
				error_msg_and_die("lost proc");
			}
			droptcb(tcp);
			return -1;
		}
		if (pollv[j].revents & POLLWANT) {
			last = j;
			return pollv[j].fd;
		}
	}
	error_msg_and_die("nothing ready");
}

static int
trace(void)
{
#ifdef POLL_HACK
	struct tcb *in_syscall = NULL;
#endif
	struct tcb *tcp;
	int pfd;
	int what;
	int ioctl_result = 0, ioctl_errno = 0;
	long arg;

	for (;;) {
		if (interactive)
			sigprocmask(SIG_SETMASK, &empty_set, NULL);

		if (nprocs == 0)
			break;

		switch (nprocs) {
		case 1:
#ifndef HAVE_POLLABLE_PROCFS
			if (proc_poll_pipe[0] == -1) {
#endif
				tcp = first_used_tcb();
				if (!tcp)
					continue;
				pfd = tcp->pfd;
				if (pfd == -1)
					continue;
				break;
#ifndef HAVE_POLLABLE_PROCFS
			}
			/* fall through ... */
#endif /* !HAVE_POLLABLE_PROCFS */
		default:
#ifdef HAVE_POLLABLE_PROCFS
#ifdef POLL_HACK
		        /* On some systems (e.g. UnixWare) we get too much ugly
			   "unfinished..." stuff when multiple proceses are in
			   syscalls.  Here's a nasty hack */

			if (in_syscall) {
				struct pollfd pv;
				tcp = in_syscall;
				in_syscall = NULL;
				pv.fd = tcp->pfd;
				pv.events = POLLWANT;
				what = poll(&pv, 1, 1);
				if (what < 0) {
					if (interrupted)
						return 0;
					continue;
				}
				else if (what == 1 && pv.revents & POLLWANT) {
					goto FOUND;
				}
			}
#endif

			if (poll(pollv, nprocs, INFTIM) < 0) {
				if (interrupted)
					return 0;
				continue;
			}
#else /* !HAVE_POLLABLE_PROCFS */
			if (proc_poll(pollv, nprocs, INFTIM) < 0) {
				if (interrupted)
					return 0;
				continue;
			}
#endif /* !HAVE_POLLABLE_PROCFS */
			pfd = choose_pfd();
			if (pfd == -1)
				continue;
			break;
		}

		/* Look up `pfd' in our table. */
		tcp = pfd2tcb(pfd);
		if (tcp == NULL) {
			error_msg_and_die("unknown pfd: %u", pfd);
		}
#ifdef POLL_HACK
	FOUND:
#endif
		/* Get the status of the process. */
		if (!interrupted) {
#ifndef FREEBSD
			ioctl_result = IOCTL_WSTOP(tcp);
#else /* FREEBSD */
			/* Thanks to some scheduling mystery, the first poller
			   sometimes waits for the already processed end of fork
			   event. Doing a non blocking poll here solves the problem. */
			if (proc_poll_pipe[0] != -1)
				ioctl_result = IOCTL_STATUS(tcp);
			else
				ioctl_result = IOCTL_WSTOP(tcp);
#endif /* FREEBSD */
			ioctl_errno = errno;
#ifndef HAVE_POLLABLE_PROCFS
			if (proc_poll_pipe[0] != -1) {
				if (ioctl_result < 0)
					kill(poller_pid, SIGKILL);
				else
					kill(poller_pid, SIGUSR1);
			}
#endif /* !HAVE_POLLABLE_PROCFS */
		}
		if (interrupted)
			return 0;

		if (interactive)
			sigprocmask(SIG_BLOCK, &blocked_set, NULL);

		if (ioctl_result < 0) {
			/* Find out what happened if it failed. */
			switch (ioctl_errno) {
			case EINTR:
			case EBADF:
				continue;
#ifdef FREEBSD
			case ENOTTY:
#endif
			case ENOENT:
				droptcb(tcp);
				continue;
			default:
				perror_msg_and_die("PIOCWSTOP");
			}
		}

#ifdef FREEBSD
		if ((tcp->flags & TCB_STARTUP) && (tcp->status.PR_WHY == PR_SYSEXIT)) {
			/* discard first event for a syscall we never entered */
			IOCTL(tcp->pfd, PIOCRUN, 0);
			continue;
		}
#endif

		/* clear the just started flag */
		tcp->flags &= ~TCB_STARTUP;

		/* set current output file */
		outf = tcp->outf;
		curcol = tcp->curcol;

		if (cflag) {
			struct timeval stime;
#ifdef FREEBSD
			char buf[1024];
			int len;

			len = pread(tcp->pfd_status, buf, sizeof(buf) - 1, 0);
			if (len > 0) {
				buf[len] = '\0';
				sscanf(buf,
				       "%*s %*d %*d %*d %*d %*d,%*d %*s %*d,%*d %*d,%*d %ld,%ld",
				       &stime.tv_sec, &stime.tv_usec);
			} else
				stime.tv_sec = stime.tv_usec = 0;
#else /* !FREEBSD */
			stime.tv_sec = tcp->status.pr_stime.tv_sec;
			stime.tv_usec = tcp->status.pr_stime.tv_nsec/1000;
#endif /* !FREEBSD */
			tv_sub(&tcp->dtime, &stime, &tcp->stime);
			tcp->stime = stime;
		}
		what = tcp->status.PR_WHAT;
		switch (tcp->status.PR_WHY) {
#ifndef FREEBSD
		case PR_REQUESTED:
			if (tcp->status.PR_FLAGS & PR_ASLEEP) {
				tcp->status.PR_WHY = PR_SYSENTRY;
				if (trace_syscall(tcp) < 0) {
					error_msg_and_die("syscall trouble");
				}
			}
			break;
#endif /* !FREEBSD */
		case PR_SYSENTRY:
#ifdef POLL_HACK
		        in_syscall = tcp;
#endif
		case PR_SYSEXIT:
			if (trace_syscall(tcp) < 0) {
				error_msg_and_die("syscall trouble");
			}
			break;
		case PR_SIGNALLED:
			if (cflag != CFLAG_ONLY_STATS
			    && (qual_flags[what] & QUAL_SIGNAL)) {
				printleader(tcp);
				tprintf("--- %s (%s) ---\n",
					signame(what), strsignal(what));
				printing_tcp = NULL;
#ifdef PR_INFO
				if (tcp->status.PR_INFO.si_signo == what) {
					printleader(tcp);
					tprints("    siginfo=");
					printsiginfo(&tcp->status.PR_INFO, 1);
					tprints("\n");
					printing_tcp = NULL;
				}
#endif
			}
			break;
		case PR_FAULTED:
			if (cflag != CFLAGS_ONLY_STATS
			    && (qual_flags[what] & QUAL_FAULT)) {
				printleader(tcp);
				tprintf("=== FAULT %d ===\n", what);
				printing_tcp = NULL;
			}
			break;
#ifdef FREEBSD
		case 0: /* handle case we polled for nothing */
			continue;
#endif
		default:
			error_msg_and_die("odd stop %d", tcp->status.PR_WHY);
			break;
		}
		/* Remember current print column before continuing. */
		tcp->curcol = curcol;
		arg = 0;
#ifndef FREEBSD
		if (IOCTL(tcp->pfd, PIOCRUN, &arg) < 0)
#else
		if (IOCTL(tcp->pfd, PIOCRUN, 0) < 0)
#endif
		{
			perror_msg_and_die("PIOCRUN");
		}
	}
	return 0;
}

#else /* !USE_PROCFS */

static int
trace(void)
{
#ifdef LINUX
	struct rusage ru;
	struct rusage *rup = cflag ? &ru : NULL;
# ifdef __WALL
	static int wait4_options = __WALL;
# endif
#endif /* LINUX */

	while (nprocs != 0) {
		int pid;
		int wait_errno;
		int status, sig;
		int stopped;
		struct tcb *tcp;
		unsigned event;

		if (interrupted)
			return 0;
		if (interactive)
			sigprocmask(SIG_SETMASK, &empty_set, NULL);
#ifdef LINUX
# ifdef __WALL
		pid = wait4(-1, &status, wait4_options, rup);
		if (pid < 0 && (wait4_options & __WALL) && errno == EINVAL) {
			/* this kernel does not support __WALL */
			wait4_options &= ~__WALL;
			pid = wait4(-1, &status, wait4_options, rup);
		}
		if (pid < 0 && !(wait4_options & __WALL) && errno == ECHILD) {
			/* most likely a "cloned" process */
			pid = wait4(-1, &status, __WCLONE, rup);
			if (pid < 0) {
				perror_msg("wait4(__WCLONE) failed");
			}
		}
# else
		pid = wait4(-1, &status, 0, rup);
# endif /* __WALL */
#endif /* LINUX */
#ifdef SUNOS4
		pid = wait(&status);
#endif
		wait_errno = errno;
		if (interactive)
			sigprocmask(SIG_BLOCK, &blocked_set, NULL);

		if (pid < 0) {
			switch (wait_errno) {
			case EINTR:
				continue;
			case ECHILD:
				/*
				 * We would like to verify this case
				 * but sometimes a race in Solbourne's
				 * version of SunOS sometimes reports
				 * ECHILD before sending us SIGCHILD.
				 */
				return 0;
			default:
				errno = wait_errno;
				perror("strace: wait");
				return -1;
			}
		}
		if (pid == popen_pid) {
			if (WIFEXITED(status) || WIFSIGNALED(status))
				popen_pid = 0;
			continue;
		}

		event = ((unsigned)status >> 16);
		if (debug) {
			char buf[sizeof("WIFEXITED,exitcode=%u") + sizeof(int)*3 /*paranoia:*/ + 16];
#ifdef LINUX
			if (event != 0) {
				static const char *const event_names[] = {
					[PTRACE_EVENT_CLONE] = "CLONE",
					[PTRACE_EVENT_FORK]  = "FORK",
					[PTRACE_EVENT_VFORK] = "VFORK",
					[PTRACE_EVENT_VFORK_DONE] = "VFORK_DONE",
					[PTRACE_EVENT_EXEC]  = "EXEC",
					[PTRACE_EVENT_EXIT]  = "EXIT",
				};
				const char *e;
				if (event < ARRAY_SIZE(event_names))
					e = event_names[event];
				else {
					sprintf(buf, "?? (%u)", event);
					e = buf;
				}
				fprintf(stderr, " PTRACE_EVENT_%s", e);
			}
#endif
			strcpy(buf, "???");
			if (WIFSIGNALED(status))
#ifdef WCOREDUMP
				sprintf(buf, "WIFSIGNALED,%ssig=%s",
						WCOREDUMP(status) ? "core," : "",
						signame(WTERMSIG(status)));
#else
				sprintf(buf, "WIFSIGNALED,sig=%s",
						signame(WTERMSIG(status)));
#endif
			if (WIFEXITED(status))
				sprintf(buf, "WIFEXITED,exitcode=%u", WEXITSTATUS(status));
			if (WIFSTOPPED(status))
				sprintf(buf, "WIFSTOPPED,sig=%s", signame(WSTOPSIG(status)));
#ifdef WIFCONTINUED
			if (WIFCONTINUED(status))
				strcpy(buf, "WIFCONTINUED");
#endif
			fprintf(stderr, " [wait(0x%04x) = %u] %s\n", status, pid, buf);
		}

		/* Look up 'pid' in our table. */
		tcp = pid2tcb(pid);

#ifdef LINUX
		/* Under Linux, execve changes pid to thread leader's pid,
		 * and we see this changed pid on EVENT_EXEC and later,
		 * execve sysexit. Leader "disappears" without exit
		 * notification. Let user know that, drop leader's tcb,
		 * and fix up pid in execve thread's tcb.
		 * Effectively, execve thread's tcb replaces leader's tcb.
		 *
		 * BTW, leader is 'stuck undead' (doesn't report WIFEXITED
		 * on exit syscall) in multithreaded programs exactly
		 * in order to handle this case.
		 *
		 * PTRACE_GETEVENTMSG returns old pid starting from Linux 3.0.
		 * On 2.6 and earlier, it can return garbage.
		 */
		if (event == PTRACE_EVENT_EXEC && os_release[0] >= '3') {
			long old_pid = 0;
			if (ptrace(PTRACE_GETEVENTMSG, pid, NULL, (long) &old_pid) >= 0
			 && old_pid > 0
			 && old_pid != pid
			) {
				struct tcb *execve_thread = pid2tcb(old_pid);
				if (tcp) {
					outf = tcp->outf;
					curcol = tcp->curcol;
					if (!cflag) {
						if (printing_tcp)
							tprints(" <unfinished ...>\n");
						printleader(tcp);
						tprintf("+++ superseded by execve in pid %lu +++\n", old_pid);
						printing_tcp = NULL;
						fflush(outf);
					}
					if (execve_thread) {
						/* swap output FILEs (needed for -ff) */
						tcp->outf = execve_thread->outf;
						execve_thread->outf = outf;
					}
					droptcb(tcp);
				}
				tcp = execve_thread;
				if (tcp) {
					tcp->pid = pid;
					tcp->flags |= TCB_REPRINT;
				}
			}
		}
#endif

		if (tcp == NULL) {
#ifdef LINUX
			if (followfork) {
				/* This is needed to go with the CLONE_PTRACE
				   changes in process.c/util.c: we might see
				   the child's initial trap before we see the
				   parent return from the clone syscall.
				   Leave the child suspended until the parent
				   returns from its system call.  Only then
				   will we have the association of parent and
				   child so that we know how to do clearbpt
				   in the child.  */
				tcp = alloctcb(pid);
				tcp->flags |= TCB_ATTACHED | TCB_STARTUP | post_attach_sigstop;
				if (!qflag)
					fprintf(stderr, "Process %d attached\n",
						pid);
			}
			else
				/* This can happen if a clone call used
				   CLONE_PTRACE itself.  */
#endif
			{
				if (WIFSTOPPED(status))
					ptrace(PTRACE_CONT, pid, (char *) 1, 0);
				error_msg_and_die("Unknown pid: %u", pid);
			}
		}
		/* set current output file */
		outf = tcp->outf;
		curcol = tcp->curcol;
#ifdef LINUX
		if (cflag) {
			tv_sub(&tcp->dtime, &ru.ru_stime, &tcp->stime);
			tcp->stime = ru.ru_stime;
		}
#endif

		if (WIFSIGNALED(status)) {
			if (pid == strace_child)
				exit_code = 0x100 | WTERMSIG(status);
			if (cflag != CFLAG_ONLY_STATS
			    && (qual_flags[WTERMSIG(status)] & QUAL_SIGNAL)) {
				printleader(tcp);
#ifdef WCOREDUMP
				tprintf("+++ killed by %s %s+++\n",
					signame(WTERMSIG(status)),
					WCOREDUMP(status) ? "(core dumped) " : "");
#else
				tprintf("+++ killed by %s +++\n",
					signame(WTERMSIG(status)));
#endif
				printing_tcp = NULL;
			}
			fflush(tcp->outf);
			droptcb(tcp);
			continue;
		}
		if (WIFEXITED(status)) {
			if (pid == strace_child)
				exit_code = WEXITSTATUS(status);
			if (tcp == printing_tcp) {
				tprints(" <unfinished ...>\n");
				printing_tcp = NULL;
			}
			if (!cflag /* && (qual_flags[WTERMSIG(status)] & QUAL_SIGNAL) */ ) {
				printleader(tcp);
				tprintf("+++ exited with %d +++\n", WEXITSTATUS(status));
				printing_tcp = NULL;
			}
			fflush(tcp->outf);
			droptcb(tcp);
			continue;
		}
		if (!WIFSTOPPED(status)) {
			fprintf(stderr, "PANIC: pid %u not stopped\n", pid);
			droptcb(tcp);
			continue;
		}

		/* Is this the very first time we see this tracee stopped? */
		if (tcp->flags & TCB_STARTUP) {
			if (debug)
				fprintf(stderr, "pid %d has TCB_STARTUP, initializing it\n", tcp->pid);
			tcp->flags &= ~TCB_STARTUP;
			if (tcp->flags & TCB_BPTSET) {
				/*
				 * One example is a breakpoint inherited from
				 * parent through fork().
				 */
				if (clearbpt(tcp) < 0) {
					/* Pretty fatal */
					droptcb(tcp);
					cleanup();
					return -1;
				}
			}
#ifdef LINUX
			if (ptrace_setoptions) {
				if (debug)
					fprintf(stderr, "setting opts %x on pid %d\n", ptrace_setoptions, tcp->pid);
				if (ptrace(PTRACE_SETOPTIONS, tcp->pid, NULL, ptrace_setoptions) < 0) {
					if (errno != ESRCH) {
						/* Should never happen, really */
						perror_msg_and_die("PTRACE_SETOPTIONS");
					}
				}
			}
#endif
		}

		sig = WSTOPSIG(status);

		if (event != 0) {
			/* Ptrace event */
#ifdef USE_SEIZE
			if (event == PTRACE_EVENT_STOP || event == PTRACE_EVENT_STOP1) {
				/*
				 * PTRACE_INTERRUPT-stop or group-stop.
				 * PTRACE_INTERRUPT-stop has sig == SIGTRAP here.
				 */
				if (sig == SIGSTOP
				 || sig == SIGTSTP
				 || sig == SIGTTIN
				 || sig == SIGTTOU
				) {
					stopped = 1;
					goto show_stopsig;
				}
			}
#endif
			goto restart_tracee_with_sig_0;
		}

		/* Is this post-attach SIGSTOP?
		 * Interestingly, the process may stop
		 * with STOPSIG equal to some other signal
		 * than SIGSTOP if we happend to attach
		 * just before the process takes a signal.
		 */
		if (sig == SIGSTOP && (tcp->flags & TCB_IGNORE_ONE_SIGSTOP)) {
			if (debug)
				fprintf(stderr, "ignored SIGSTOP on pid %d\n", tcp->pid);
			tcp->flags &= ~TCB_IGNORE_ONE_SIGSTOP;
			goto restart_tracee_with_sig_0;
		}

		if (sig != syscall_trap_sig) {
			siginfo_t si;

			/* Nonzero (true) if tracee is stopped by signal
			 * (as opposed to "tracee received signal").
			 */
			stopped = (ptrace(PTRACE_GETSIGINFO, pid, 0, (long) &si) < 0);
#ifdef USE_SEIZE
 show_stopsig:
#endif
			if (cflag != CFLAG_ONLY_STATS
			    && (qual_flags[sig] & QUAL_SIGNAL)) {
#if defined(PT_CR_IPSR) && defined(PT_CR_IIP)
				long pc = 0;
				long psr = 0;

				upeek(tcp, PT_CR_IPSR, &psr);
				upeek(tcp, PT_CR_IIP, &pc);

# define PSR_RI	41
				pc += (psr >> PSR_RI) & 0x3;
# define PC_FORMAT_STR	" @ %lx"
# define PC_FORMAT_ARG	, pc
#else
# define PC_FORMAT_STR	""
# define PC_FORMAT_ARG	/* nothing */
#endif
				printleader(tcp);
				if (!stopped) {
					tprints("--- ");
					printsiginfo(&si, verbose(tcp));
					tprintf(" (%s)" PC_FORMAT_STR " ---\n",
						strsignal(sig)
						PC_FORMAT_ARG);
				} else
					tprintf("--- %s by %s" PC_FORMAT_STR " ---\n",
						strsignal(sig),
						signame(sig)
						PC_FORMAT_ARG);
				printing_tcp = NULL;
				fflush(tcp->outf);
			}

			if (!stopped)
				/* It's signal-delivery-stop. Inject the signal */
				goto restart_tracee;

			/* It's group-stop */
#ifdef USE_SEIZE
			if (use_seize) {
				/*
				 * This ends ptrace-stop, but does *not* end group-stop.
				 * This makes stopping signals work properly on straced process
				 * (that is, process really stops. It used to continue to run).
				 */
				if (ptrace_restart(PTRACE_LISTEN, tcp, 0) < 0) {
					cleanup();
					return -1;
				}
				continue;
			}
			/* We don't have PTRACE_LISTEN support... */
#endif
			goto restart_tracee;
		}

		/* We handled quick cases, we are permitted to interrupt now. */
		if (interrupted)
			return 0;

		/* This should be syscall entry or exit.
		 * (Or it still can be that pesky post-execve SIGTRAP!)
		 * Handle it.
		 */
		if (trace_syscall(tcp) < 0 && !tcp->ptrace_errno) {
			/* ptrace() failed in trace_syscall() with ESRCH.
			 * Likely a result of process disappearing mid-flight.
			 * Observed case: exit_group() terminating
			 * all processes in thread group.
			 */
			if (tcp->flags & TCB_ATTACHED) {
				if (printing_tcp) {
					/* Do we have dangling line "syscall(param, param"?
					 * Finish the line then.
					 */
					printing_tcp->flags |= TCB_REPRINT;
					tprints(" <unfinished ...>\n");
					printing_tcp = NULL;
					fflush(tcp->outf);
				}
				/* We assume that ptrace error was caused by process death.
				 * We used to detach(tcp) here, but since we no longer
				 * implement "detach before death" policy/hack,
				 * we can let this process to report its death to us
				 * normally, via WIFEXITED or WIFSIGNALED wait status.
				 */
			} else {
				/* It's our real child (and we also trace it) */
				/* my_tkill(pid, SIGKILL); - why? */
				/* droptcb(tcp); - why? */
			}
			continue;
		}
 restart_tracee_with_sig_0:
		sig = 0;
 restart_tracee:
		/* Remember current print column before continuing. */
		tcp->curcol = curcol;
		if (ptrace_restart(PTRACE_SYSCALL, tcp, sig) < 0) {
			cleanup();
			return -1;
		}
	}
	return 0;
}

#endif /* !USE_PROCFS */

void
tprintf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	if (outf) {
		int n = vfprintf(outf, fmt, args);
		if (n < 0) {
			if (outf != stderr)
				perror(outfname == NULL
				       ? "<writing to pipe>" : outfname);
		} else
			curcol += n;
	}
	va_end(args);
}

void
tprints(const char *str)
{
	if (outf) {
		int n = fputs(str, outf);
		if (n >= 0) {
			curcol += strlen(str);
			return;
		}
		if (outf != stderr)
			perror(outfname == NULL
			       ? "<writing to pipe>" : outfname);
	}
}

void
printleader(struct tcb *tcp)
{
	if (printing_tcp) {
		if (printing_tcp->ptrace_errno) {
			if (printing_tcp->flags & TCB_INSYSCALL) {
				tprints(" <unavailable>) ");
				tabto();
			}
			tprints("= ? <unavailable>\n");
			printing_tcp->ptrace_errno = 0;
		} else if (!outfname || followfork < 2 || printing_tcp == tcp) {
			printing_tcp->flags |= TCB_REPRINT;
			tprints(" <unfinished ...>\n");
		}
	}

	printing_tcp = tcp;
	curcol = 0;
	if ((followfork == 1 || pflag_seen > 1) && outfname)
		tprintf("%-5d ", tcp->pid);
	else if (nprocs > 1 && !outfname)
		tprintf("[pid %5u] ", tcp->pid);
	if (tflag) {
		char str[sizeof("HH:MM:SS")];
		struct timeval tv, dtv;
		static struct timeval otv;

		gettimeofday(&tv, NULL);
		if (rflag) {
			if (otv.tv_sec == 0)
				otv = tv;
			tv_sub(&dtv, &tv, &otv);
			tprintf("%6ld.%06ld ",
				(long) dtv.tv_sec, (long) dtv.tv_usec);
			otv = tv;
		}
		else if (tflag > 2) {
			tprintf("%ld.%06ld ",
				(long) tv.tv_sec, (long) tv.tv_usec);
		}
		else {
			time_t local = tv.tv_sec;
			strftime(str, sizeof(str), "%T", localtime(&local));
			if (tflag > 1)
				tprintf("%s.%06ld ", str, (long) tv.tv_usec);
			else
				tprintf("%s ", str);
		}
	}
	if (iflag)
		printcall(tcp);
}

void
tabto(void)
{
	if (curcol < acolumn)
		tprints(acolumn_spaces + curcol);
}

#ifdef HAVE_MP_PROCFS

int
mp_ioctl(int fd, int cmd, void *arg, int size)
{
	struct iovec iov[2];
	int n = 1;

	iov[0].iov_base = &cmd;
	iov[0].iov_len = sizeof cmd;
	if (arg) {
		++n;
		iov[1].iov_base = arg;
		iov[1].iov_len = size;
	}

	return writev(fd, iov, n);
}

#endif

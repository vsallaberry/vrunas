/*
 * Copyright (C) 2018-2020 Vincent Sallaberry
 * vrunas <https://github.com/vsallaberry/vrunas>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * -------------------------------------------------------------------------
 * main
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef HAVE_VERSION_H
# include "version.h"
#endif

#include "vlib/options.h"
#include "vlib/time.h"
#include "vlib/account.h"
#include "vlib/log.h"
#include "vlib/logpool.h"
#include "vlib/util.h"
#include "vlib/term.h"

#define VERSION_STRING(lic) lic(BUILD_APPNAME, APP_VERSION, \
                                "git:" BUILD_GITREV, "Vincent Sallaberry", "2018-2020")

static const opt_options_desc_t s_opt_desc[] = {
    { OPT_ID_SECTION, NULL, "options", "\nOptions:" },
    { 'h', "help",          "[filter[,...]]","summary or full usage of filter, use '-hh'\r" },
    { 'V', "version",       NULL,           "show version" },
    { 's', "source",        "[[:]pattern]", "show source - pattern: <project/file> or :<text> "
                            "(fnmatch(3) shell pattern)." },
    { 'l', "log-level",     "level",        "Set log level "
                                            "[mod1=]lvl1[@file1][:flag1[|..]][,..]\r" },
    { 'u', "user",          "uid|user",     "change uid" },
    { 'g', "group",         "gid|group",    "change gid" },
    { 'U', "print-uid",     "user",         "print uid of user, no program/arguments required." },
    { 'G', "print-gid",     "group",        "print gid of group, no program/arguments required." },
    { '1', "to-stdout",     NULL,           "redirect program stderr to stdout" },
    { '2', "to-stderr",     NULL,           "redirect program stdout to stderr" },
        /* "  -1|-2        : redirect program stderr or stdout to respectively stdout(-1) or stderr(-2)" */
    { 't', "time",          NULL,           "print timings of program ('time -p' POSIX format).\r"
                                            "With -1: timings will be printed to stderr,\r"
                                            "With -2: to stdout, otherwise, to stderr.\rTo put timings in "
                                            "variable and display command: '$ t=`vrunas -2 -t ls -R /`'" },
    { 'T', "time-extended", NULL,           "same as -t/--time but with extended format." },
        /* "  -t|-T        : print timings of program (-t:'time -p' POSIX, -T:extended)\n"
            "                 With -1: timings will be printed to stderr.\n"
            "                 With -2: to stdout, otherwise, to stderr. To put timings in\n"
            "                 variable and display command: '$ t=`vrunas -2 -t ls -R /`'\n" */
    { 'o', "output",        "file",         "redirect program stdout to file.\r"
                                            "With -1 or -2, program stderr AND stdout are redirected to file" },
    { 'O', "append-to",     "file",         "same as -o/--output but append to file" },
        /*  "  -o|-O file   : redirect program stdout to file (-O:append).\n"
            "                 With -1 or -2, program stderr AND stdout are redirected to file\n" */
    { 'N', "new-identity",  NULL,           "create/open in/out file with New identity, after uid/gid switch" },
    { 'i', "input",         "file",         "program receives input from file instead of stdin." },
    { 'p', "priority",      "priority",     "set program priority (nice value from -20 to 20)." },
#   ifdef _TEST
    /* nothing */
#   endif
    { OPT_ID_SECTION, NULL, "arguments", "\nArguments:" },
    { OPT_ID_ARG, NULL, "[program [arguments]]", "program and arguments, required unless -U/-G is given" },
    { OPT_ID_END, NULL, NULL, NULL }
};

enum FLAGS {
    HAVE_UID        = 1 << 0,
    HAVE_GID        = 1 << 1,
    OPTIONAL_ARGS   = 1 << 2,
    TO_STDOUT       = 1 << 3,
    TO_STDERR       = 1 << 4,
    OUT_APPEND      = 1 << 5,
    TIME_POSIX      = 1 << 6,
    TIME_EXT        = 1 << 7,
    WARN_MOREREDIRS = 1 << 8,
    FILE_NEWIDENTITY= 1 << 9,
    HAVE_PRIORITY   = 1 << 10,
};

enum {
    OK                  = 0,
    ERR_PROG_MISSING    = 1,
    ERR_SETID           = 2,
    ERR_BUILDARGV       = 4,
    ERR_EXEC            = 5,
    ERR_REDIR           = 6,
    ERR_SETOUT          = 7,
    ERR_BENCH           = 8,
    ERR_SETIN           = 9,
    ERR_PRIORITY        = 10,
    ERR_OPTION          = 30,
    ERR                 = -1,
    ERR_NOT_REACHABLE   = -128,
};

typedef struct {
    logpool_t *         logs;
    int                 flags;
    int                 argc;
    char *const*        argv;
    char *              buf;
    size_t              bufsz;
    FILE *              alternatefile;  /* file not used for application output, can be used to display bench */
    int                 outfd;          /* fd of file receving program output, -1 if stdout or stderr */
    int                 infd;           /* fd of file replacing program input, -1 if stdin */
    const char *        outfile;
    const char *        infile;
    uid_t               uid;
    gid_t               gid;
    int                 priority;
    int                 i_argv_program;
} ctx_t;

static int clean_ctx(int ret, ctx_t * ctx) {
    if (ctx) {
        vterm_enable(0);
        if (ctx->logs != NULL) {
            logpool_free(ctx->logs);
        }
        if (ctx->buf) {
            free(ctx->buf);
            ctx->buf = NULL;
        }
        if (ctx->alternatefile != NULL) {
            fclose(ctx->alternatefile);
            ctx->alternatefile = NULL;
        }
        if (ctx->outfd >= 0) {
            close(ctx->outfd);
            ctx->outfd = -1;
        }
        if (ctx->infd >= 0) {
            close(ctx->infd);
            ctx->infd = -1;
        }
    }
    return ret;
}

int set_uidgid(uid_t uid, gid_t gid, ctx_t * ctx) {
    int errno_bak;

    /* set gid if given */
    if ((ctx->flags & HAVE_GID) != 0) {
        if (setgid(gid) < 0) {
            errno_bak = errno;
            vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
            fprintf(stderr, "error%s: `%lu` (setgid): %s\n",
                    vterm_color(STDERR_FILENO, VCOLOR_RESET), (unsigned long) gid, strerror(errno_bak));
            return ERR_SETID;
        }
    }
    /* set uid if given */
    if ((ctx->flags & HAVE_UID) != 0) {
        if (setuid(uid) < 0) {
            errno_bak = errno;
            vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
            fprintf(stderr, "error%s: `%lu` (setuid): %s\n",
                    vterm_color(STDERR_FILENO, VCOLOR_RESET), (unsigned long) uid, strerror(errno_bak));
            return ERR_SETID;
        }
    }
    return 0;
}

char ** build_argv(int argc, char * const * argv, ctx_t * ctx) {
    char ** newargv, ** tmp;
    int errno_bak;
    (void)ctx;

    if ((tmp = newargv = malloc(argc + 1)) == NULL) {
        errno_bak = errno;
        vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
        fprintf(stderr, "error%s: build_argv(malloc) : %s\n",
                vterm_color(STDERR_FILENO, VCOLOR_RESET), strerror(errno_bak));
        return NULL;
    }
    while (argc--)  {
        *(tmp++) = *(argv++);
    }
    *tmp = NULL;
    return newargv;
}

int set_redirections(ctx_t * ctx) {
    int     backupfd;
    int     dupfd;
    int     redirectedfd = -1;
    int     ret = 0;

    /* This method sets up stdout/stderr redirections so that the -1,-2 options
     * are taken into account, AND, so that in case the '-t/-T' options are given,
     * timings are the only things displayed on a given output (stderr|stdout) to
     * make it easy to catch in shell scripts.
     * Therefore, in this method we have to take care when displaying anything,
     * because redirections are not set up yet */

    /* With '-2', stdout is redirected to stderr. If bench is ON,
     * it is displayed on the real stdout (ctx->alternatefile) */
    if ((ctx->flags & TO_STDERR) != 0) {
        dupfd = STDERR_FILENO;
        redirectedfd = STDOUT_FILENO;
    }
    /* Else, with '-1' or if bench is ON, stderr is redirected to stdout, and
     * bench is displayed on the real stderr (ctx->alternatefile) */
    else if ((ctx->flags & TO_STDOUT) != 0  || (ctx->flags & (TIME_POSIX | TIME_EXT)) != 0) {
        dupfd = STDOUT_FILENO;
        redirectedfd = STDERR_FILENO;
    }
    if (redirectedfd >= 0) {
        /* make a backup of redirected fd */
        if ((backupfd = dup(redirectedfd)) < 0) {
            ret = -1;
        }
        /* redirect redirected fd on dupfdt */
        else if (dup2(dupfd, redirectedfd) < 0) {
            ret = -2;
        }
        /* make a FILE * of backupfd */
        else if ((ctx->alternatefile = fdopen(backupfd, "w")) == NULL) {
            ret = -3;
        }
    }
    return ret;
}

int set_out(const char * file, ctx_t * ctx) {
    int open_flags = O_WRONLY | O_CREAT;
    int fd;
    int errno_bak;

    if (file == NULL)
        return 0;

    if ((ctx->flags & OUT_APPEND) != 0)
        open_flags |= O_APPEND;
    else
        open_flags |= O_TRUNC;

    if ((fd = open(file, open_flags, (S_IWUSR | S_IRUSR | S_IRGRP /* | S_IROTH */ ))) < 0) {
        errno_bak = errno;
        /* error */
        vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
        fprintf(stderr, "error%s: set_out(open), %s: %s\n",
                vterm_color(STDERR_FILENO, VCOLOR_RESET), file, strerror(errno_bak));
        return -1;
    }

    /* redirect always stdout to file. redirect stderr if -1 or -2 is given */
    if ((ctx->flags & (TO_STDERR | TO_STDOUT)) != 0 && dup2(fd, STDERR_FILENO) < 0) {
        errno_bak = errno;
        vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
        fprintf(stderr, "error%s: set_out(dup2 stderr): %s\n",
                vterm_color(STDERR_FILENO, VCOLOR_RESET), strerror(errno_bak));
        close(fd);
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) < 0) {
        errno_bak = errno;
        vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
        fprintf(stdout, "error%s: set_out(dup2 stdout): %s\n",
                vterm_color(STDERR_FILENO, VCOLOR_RESET), strerror(errno_bak));
        close(fd);
        return -1;
    }
    return fd;
}

int set_in(const char * file, ctx_t * ctx) {
    int fd;
    int errno_bak;
    (void) ctx;

    if (file == NULL)
        return 0;
    if ((fd = open(file, O_RDONLY)) < 0) {
        errno_bak = errno;
        vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
        fprintf(stderr, "error%s: set_in(open): %s\n",
                vterm_color(STDERR_FILENO, VCOLOR_RESET), strerror(errno_bak));
        return -1;
    }
    if (dup2(fd, STDIN_FILENO) < 0) {
        errno_bak = errno;
        vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
        fprintf(stderr, "error%s: set_in(dup2 stdin): %s\n",
                vterm_color(STDERR_FILENO, VCOLOR_RESET), strerror(errno_bak));
        close(fd);
        return -1;
    }
    return fd;
}

/* signal handler for do_bench(), ignoring and forwarding signals to child */
static void sig_handler(int sig) {
    static pid_t pid = 0;
    if (pid == 0) {
        pid = (pid_t) sig;
        return ;
    }
    kill(pid, sig);
}

static int do_bench(ctx_t * ctx) {
    if ((ctx->flags & (TIME_POSIX | TIME_EXT)) != 0) {
        pid_t           wpid, pid;
        struct timespec ts0;

        if (vclock_gettime(CLOCK_MONOTONIC_RAW, &ts0) < 0) {
            fprintf(stderr, "bench: vclock_gettime#1 error: %s\n", strerror(errno));
            memset(&ts0, 0, sizeof(ts0));
        }
        if ((pid = fork()) < 0) {
            perror("fork");
            return ERR_BENCH;
        } else if (pid == 0) {
            /* son : give to hand to father, and continue execution */
            sched_yield();
            return 0;
        } else {
            /* father */
            struct          rusage rusage;
            FILE *          out = ctx->alternatefile;
            int             status;
            struct timespec ts1;
            int     sigs[] = { SIGINT, SIGHUP, SIGTERM, SIGQUIT, SIGUSR1, SIGUSR2, SIGPIPE };
            struct sigaction sa = { .sa_handler = sig_handler, .sa_flags = SA_RESTART };

            /* install signal handler and give to him the program pid */
            sig_handler(pid);
            sigemptyset(&sa.sa_mask);
            for (unsigned int i = 0; i < sizeof(sigs) / sizeof(*sigs); i++) {
                if (sigaction(sigs[i], &sa, NULL) < 0)
                    fprintf(stderr, "bench sigaction(%s): %s\n", strsignal(sigs[i]), strerror(errno));
            }

            /* wait for termination of program */
            if ((wpid = waitpid(pid, &status, 0 /* options */)) <= 0)
                perror("waitpid");

            /* get timings and other stats */
            if (vclock_gettime(CLOCK_MONOTONIC_RAW, &ts1) < 0) {
                fprintf(stderr, "bench: vclock_gettime#2 error: %s\n", strerror(errno));
                memset(&ts1, 0, sizeof(ts1));
            }
            vtimespecsub(&ts1, &ts0, &ts1);

            if (getrusage(RUSAGE_CHILDREN, &rusage) < 0)
                perror("getrusage");

            if ((ctx->flags & TIME_POSIX) != 0) {
                fprintf(out, "real %ld.%02d\nuser %ld.%02d\nsys %ld.%02d\n",
                        (long)ts1.tv_sec, (int)(ts1.tv_nsec / 10000000),
                        (long)rusage.ru_utime.tv_sec, (int)(rusage.ru_utime.tv_usec / 10000),
                        (long)rusage.ru_stime.tv_sec, (int)(rusage.ru_stime.tv_usec / 10000));
            }

            if ((ctx->flags & TIME_EXT) != 0) {
                /* ru_utime     the total amount of time spent executing in user mode.
                   ru_stime     the total amount of time spent in the system executing on behalf of the process(es). */
                fprintf(out, "realtime % 3ld.%09d (the real time in seconds spent by process with nsec precision)\n"
                             "maxrss   % 13ld (the maximum resident set size utilized (in bytes).)\n"
                             "ixrss    % 13ld (an integral value indicating the amount of memory used "
                                              "by the text segment that was also shared among other "
                                              "processes. This value is expressed in units of "
                                              "kilobytes * ticks-of-execution.)\n"
                             "idrss    % 13ld (an integral value of the amount of unshared memory residing "
                                              "in the data segment of a process (expressed in units of "
                                              "kilobytes * ticks-of-execution).\n"
                             "isrss    % 13ld (an integral value of the amount of unshared memory residing "
                                              "in the stack segment of a process (expressed in units of "
                                              "kilobytes * ticks-of-execution).)\n"
                             "minflt   % 13ld (the number of page faults serviced without any I/O activity; "
                                              "here I/O activity is avoided by reclaiming a page frame from "
                                              "the list of pages awaiting reallocation.)\n"
                             "majflt   % 13ld (the number of page faults serviced that required I/O activity.)\n"
                             "nswap    % 13ld (the number of times a process was swapped out of main memory.)\n"
                             "inblock  % 13ld (the number of times the file system had to perform input.)\n"
                             "oublock  % 13ld (the number of times the file system had to perform output.)\n"
                             "msgsnd   % 13ld (the number of IPC messages sent.)\n"
                             "msgrcv   % 13ld (the number of IPC messages received.)\n"
                             "nsignals % 13ld (the number of signals delivered.)\n"
                             "ncvsw    % 13ld (the number of times a context switch resulted due to a process "
                                              "voluntarily giving up the processor before its time slice was "
                                              "completed (usually to await availability of a resource).)\n"
                             "nivcsw   % 13ld (the number of times a context switch resulted due to a higher "
                                              "priority process becoming runnable or because the current "
                                              "process exceeded its time slice.)\n",
                        (long) ts1.tv_sec, (int) ts1.tv_nsec / 1000,
                        rusage.ru_maxrss, rusage.ru_ixrss, rusage.ru_idrss, rusage.ru_isrss,
                        rusage.ru_minflt, rusage.ru_majflt,
                        rusage.ru_nswap,
                        rusage.ru_inblock, rusage.ru_oublock,
                        rusage.ru_msgsnd, rusage.ru_msgrcv,
                        rusage.ru_nsignals,
                        rusage.ru_nvcsw, rusage.ru_nivcsw
                        );
            }

            /* Terminate with child status */
            if (WIFEXITED(status)) {
                exit(clean_ctx(WEXITSTATUS(status), ctx));
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "child terminated by signal %d\n", WTERMSIG(status));
                exit(clean_ctx(-100-WTERMSIG(status), ctx));
            } else {
                fprintf(stderr, "child terminated by ?\n");
                exit(clean_ctx(-100, ctx));
            }
        }
    }
    return 0;
}

/** parse_option_first_pass() : option callback of type opt_option_callback_t. see vlib/options.h */
static int parse_option_first_pass(int opt, const char *arg, int *i_argv, opt_config_t * opt_config) {
    ctx_t * ctx = opt_config ? (ctx_t *) opt_config->user_data : NULL;
    log_t * log;
    (void) i_argv;
    if (ctx == NULL)
        return OPT_ERROR(ERR_OPTION);
    switch (opt) {
        case 't': ctx->flags |= TIME_POSIX;  break ;
        case 'T': ctx->flags |= TIME_EXT;    break ;
        case '1':
            if ((ctx->flags & TO_STDERR) != 0)
                ctx->flags |= WARN_MOREREDIRS;
            ctx->flags = (ctx->flags & ~TO_STDERR) | TO_STDOUT;
            break ;
        case '2':
            if ((ctx->flags & TO_STDOUT) != 0)
                ctx->flags |= WARN_MOREREDIRS;
                ctx->flags = (ctx->flags & ~TO_STDOUT) | TO_STDERR;
            break ;
        case 'l':
            if ((ctx->logs = logpool_create_from_cmdline(ctx->logs, arg, NULL)) == NULL)
                return OPT_ERROR(OPT_EBADARG);
            break ;
        case OPT_ID_END:
            if ((log = logpool_getlog(ctx->logs, "vlib", LPG_NODEFAULT | LPG_TRUEPREFIX)) != NULL) {
                log_set_vlib_instance(log);
            } else {
                 log_t vlog = { .level = LOG_LVL_INFO, .out = stderr, .flags = LOG_FLAG_NONE, .prefix = NULL };
                 log_set_vlib_instance(logpool_add(ctx->logs, &vlog, NULL));
            }
            opt_config->log = logpool_getlog(ctx->logs, "options", LPG_NODEFAULT | LPG_TRUEPREFIX);
            /* setup of setout/stderr redirections so that we can use them blindly */
            if (set_redirections(ctx) != 0) {
                /* see comment inside set_redirections() method. Safest thing is to not display anything
                 * on error. Error here is rare, but... TODO */
                fprintf((ctx->flags & (TIME_POSIX | TIME_EXT)) == 0
                        ? stderr : (ctx->flags & TO_STDERR) != 0 ? stdout : stderr,
                        "set_redirections(dup|dup2|open): %s\n", strerror(errno));
                exit(clean_ctx(ERR_REDIR, ctx));
            }
            if ((ctx->flags & WARN_MOREREDIRS) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_YELLOW, VCOLOR_EMPTY, VCOLOR_EMPTY));
                fprintf(stderr, "warning%s, conflicting '-1' and '-2' options, taking the last one: '%s'\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET),
                        (ctx->flags & TO_STDERR) != 0 ? "-2" : "-1");
            }
            break ;
        case OPT_ID_ARG:
            *i_argv = opt_config->argc;
            break ;
    }
    return OPT_CONTINUE(0);
}

/** parse_option() : option callback of type opt_option_callback_t. see vlib/options.h */
static int parse_option(int opt, const char *arg, int *i_argv, opt_config_t * opt_config) {
    const char * const  modules[]   = { "vlib", "options", "*", NULL };
    ctx_t *             ctx         = opt_config ? (ctx_t *) opt_config->user_data : NULL;
    (void) arg;
    (void) i_argv;

    if (ctx == NULL)
        return OPT_ERROR(ERR_OPTION);
    if ((opt & OPT_DESCRIBE_OPTION) != 0) {
        switch(opt & OPT_OPTION_FLAG_MASK) {
            case 'h':
                return opt_describe_filter(opt, arg, i_argv, opt_config);
            case 'l':
                return log_describe_option((char *)arg, i_argv, modules, NULL, NULL);
        }
        return OPT_EXIT_OK(0);
    }
    switch (opt) {
        char *  endptr = NULL;
        uid_t   tmpuid;
        gid_t   tmpgid;
        int     tmp;
        case 'p':
            errno = 0;
            tmp = strtol(arg, &endptr, 0);
            if (errno != 0 || *endptr != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
                fprintf(stderr, "error%s, bad priority '%s'\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
                return OPT_ERROR(ERR_OPTION+11);
            }
            if ((ctx->flags & HAVE_PRIORITY) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_YELLOW, VCOLOR_EMPTY, VCOLOR_EMPTY));
                fprintf(stderr, "warning%s, overriding previous priority '%d' with new value '%d'\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), ctx->priority, tmp);
            }
            ctx->priority = tmp;
            ctx->flags |= HAVE_PRIORITY;
            break ;
        case 'i':
            if (ctx->infile != NULL) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_YELLOW, VCOLOR_EMPTY, VCOLOR_EMPTY));
                fprintf(stderr, "warning%s, overriding previous '-%c %s' with '-%c %s'\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), opt, ctx->infile, opt, arg);
            }
            ctx->infile = arg;
            break ;
        case 'N': ctx->flags |= FILE_NEWIDENTITY; break ;
        case 'o':
        case 'O':
            if (ctx->outfile != NULL) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_YELLOW, VCOLOR_EMPTY, VCOLOR_EMPTY));
                fprintf(stderr, "warning%s, overriding previous '-%c %s' with '-%c %s'\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET),
                        (ctx->flags & OUT_APPEND) != 0 ? 'O' : 'o', ctx->outfile, opt, arg);
            }
            if (opt == 'O')
                ctx->flags |= OUT_APPEND;
            else
                ctx->flags &= ~OUT_APPEND;
            ctx->outfile = arg;
            break ;
        case 'u':
            if ((ctx->flags & HAVE_UID) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_YELLOW, VCOLOR_EMPTY, VCOLOR_EMPTY));
                fprintf(stderr, "warning%s, overriding previous `-u` parameter with new value `%s`\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
            }
            errno = 0;
            tmpuid = strtol(arg, &endptr, 0);
            if ((errno != 0 || !endptr || *endptr != 0)
            &&  pwfindid_r(arg, &tmpuid, &ctx->buf, &ctx->bufsz) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
                fprintf(stderr, "error%s: pwfindid_r(%s): invalid user\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
                return OPT_ERROR(ERR_OPTION+7);
            }
            ctx->flags |= HAVE_UID;
            ctx->uid = tmpuid;
            break ;
        case 'U':
            if (pwfindid_r(arg, &tmpuid, &ctx->buf, &ctx->bufsz) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
                fprintf(stderr, "error%s: pwfindid_r(%s): invalid user\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
                return OPT_ERROR(ERR_OPTION+5);
            }
            ctx->flags |= OPTIONAL_ARGS;
            fprintf(stdout, "%d\n", (int) tmpuid);
            break ;
        case 'g':
            if ((ctx->flags & HAVE_GID) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_YELLOW, VCOLOR_EMPTY, VCOLOR_EMPTY));
                fprintf(stderr, "warning%s, overriding previous `-g` parameter with new value `%s`\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
            }
            errno = 0;
            tmpgid = strtol(arg, &endptr, 0);
            if ((errno != 0 || !endptr || *endptr != 0)
            &&  grfindid_r(arg, &tmpgid, &ctx->buf, &ctx->bufsz) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
                fprintf(stderr, "error%s: grfindid_r(%s): invalid group\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
                return OPT_ERROR(ERR_OPTION+3);
            }
            ctx->flags |= HAVE_GID;
            ctx->gid = tmpgid;
            break ;
        case 'G':
            if (grfindid_r(arg, &tmpgid, &ctx->buf, &ctx->bufsz) != 0) {
                vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
                fprintf(stderr, "error%s: grfindid_r(%s): invalid group\n",
                        vterm_color(STDERR_FILENO, VCOLOR_RESET), arg);
                return OPT_ERROR(ERR_OPTION+1);
            }
            ctx->flags |= OPTIONAL_ARGS;
            fprintf(stdout, "%d\n", (int) tmpgid);
            break ;
        case 's':
            opt_filter_source(stdout, arg, vrunas_get_source, vlib_get_source, NULL);
            return OPT_EXIT_OK(0);
#       ifdef _TEST
        case 'd': break ;
#       endif
        case 'V':
            fprintf(stdout, "%s\n\nWith:\n  %s\n\n", VERSION_STRING(OPT_VERSION_STRING_GPL3PLUS_L), vlib_get_version());
            return OPT_EXIT_OK(0);
        case 'h': return opt_usage(OPT_EXIT_OK(0), opt_config, arg);
        case OPT_ID_ARG:
            ctx->i_argv_program = *i_argv;
            *i_argv = opt_config->argc;
            break ;
    }
    return OPT_CONTINUE(0);
}

int main(int argc, char *const* argv) {
    ctx_t           ctx = {
        .flags = 0, .argc = argc, .argv = argv, .buf = NULL, .bufsz = 0,
        .alternatefile = NULL, .outfd = -1, .infd = -1, .outfile = NULL, .infile = NULL,
        .logs = logpool_create(), .uid = 0, .gid = 0, .priority = 0, .i_argv_program = 0,
    };
    opt_config_t    opt_config  = OPT_INITIALIZER(argc, argv, parse_option_first_pass, s_opt_desc,
                                                  VERSION_STRING(OPT_VERSION_STRING_GPL3PLUS), &ctx);
    char **         newargv = NULL;
    int             ret = 0;
    int             errno_bak;

    /* Manage program options: first pass on command line to set redirections, in silent mode:
     * nothing has to be written on stdout/stderr until set_redirections() is called */
    if (OPT_IS_EXIT(ret = opt_parse_options_2pass(&opt_config, parse_option))) {
        return clean_ctx(OPT_EXIT_CODE(ret), &ctx);
    }

    /* clean now unnecessary resources */
    if (ctx.buf) {
        free(ctx.buf);
        ctx.buf = NULL;
    }
    do {
        /* error if program is mandatory */
        if (ctx.i_argv_program == 0 || ctx.i_argv_program >= argc) {
            if ((ctx.flags & OPTIONAL_ARGS) != 0 && ((ret = 0) || 1))
                break ;
            vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
            fprintf(stderr, "error%s: missing program\n", vterm_color(STDERR_FILENO, VCOLOR_RESET));
            ret = opt_usage(OPT_ERROR(ERR_PROG_MISSING), &opt_config, NULL);
            break ;
        }
        /* program header */
        fprintf(stdout, "%s\n\n", opt_config.version_string);
        /* prepare priority, uid, gid, newargv, outfile, bench for excvp */
        if ((ctx.flags & HAVE_PRIORITY) != 0 && setpriority(PRIO_PROCESS, getpid(), ctx.priority) < 0) {
            errno_bak = errno;
            ret = ERR_PRIORITY;
            vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
            fprintf(stderr, "error%s: setpriority(%d): %s\n",
                    vterm_color(STDERR_FILENO, VCOLOR_RESET), ctx.priority, strerror(errno_bak));
            break ;
        }
        if ((ctx.flags & FILE_NEWIDENTITY) != 0 && set_uidgid(ctx.uid, ctx.gid, &ctx) != 0 && ((ret = ERR_SETID) || 1))
            break ;
        if ((ctx.outfd = set_out(ctx.outfile, &ctx)) < 0 && ((ret = ERR_SETOUT) || 1))
            break ;
        if ((ctx.infd = set_in(ctx.infile, &ctx)) < 0 && ((ret = ERR_SETIN) || 1))
            break ;
        if ((ctx.flags & FILE_NEWIDENTITY) == 0 && set_uidgid(ctx.uid, ctx.gid, &ctx) != 0 && ((ret = ERR_SETID) || 1))
            break ;
        if (do_bench(&ctx) != 0 && ((ret = ERR_BENCH) || 1))
            break ;
        if ((newargv = build_argv(argc - ctx.i_argv_program, argv + ctx.i_argv_program, &ctx)) == NULL && ((ret = ERR_BUILDARGV) || 1))
            break ;
        /* execvp, in, if needed, a forked process */
        if (execvp(*newargv, newargv) < 0) {
            errno_bak = errno;
            ret = ERR_EXEC;
            vterm_putcolor(stderr, VCOLOR_BUILD(VCOLOR_RED, VCOLOR_EMPTY, VCOLOR_BOLD));
            fprintf(stderr, "error%s: `%s` (execvp): %s\n",
                    vterm_color(STDERR_FILENO, VCOLOR_RESET), *newargv, strerror(errno_bak));
            break ;
        }
        /* not reachable */
        return ERR_NOT_REACHABLE;
    } while (0);
    if (newargv)
        free(newargv);
    return clean_ctx(ret, &ctx);
}

#ifndef APP_INCLUDE_SOURCE
# define APP_NO_SOURCE_STRING "\n/* #@@# FILE #@@# " BUILD_APPNAME "/* */\n" \
                              BUILD_APPNAME " source not included in this build.\n"
int vrunas_get_source(FILE * out, char * buffer, unsigned int buffer_size, void ** ctx) {
    return vdecode_buffer(out, buffer, buffer_size, ctx,
           APP_NO_SOURCE_STRING, sizeof(APP_NO_SOURCE_STRING) - 1);
}
#endif


/*
 * Copyright (C) 2018 Vincent Sallaberry
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
#include <sys/times.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#ifdef HAVE_VERSION_H
# include "version.h"
#endif

enum FLAGS {
    HAVE_UID        = 1 << 0,
    HAVE_GID        = 1 << 1,
    OPTIONAL_ARGS   = 1 << 2,
    TO_STDOUT       = 1 << 3,
    TO_STDERR       = 1 << 4,
    OUT_APPEND      = 1 << 5,
    TIME            = 1 << 6,
    TIMEEXT         = 1 << 7,
};

enum {
    OK = 0,
    ERR_PROG_MISSING = 1,
    ERR_SETID = 2,
    ERR_BUILDARGV = 4,
    ERR_EXEC = 5,
    ERR_REDIR = 6,
    ERR_SETOUT = 7,
    ERR_BENCH = 8,
    ERR_OPTION = 10,
    ERR = -1,
    ERR_NOT_REACHABLE = -128,
};

typedef struct {
    int                 flags;
    int                 argc;
    char *const*        argv;
    char *              buf;
    size_t              bufsz;
    FILE *              alternatefile;  /* file not used for application output, can be used to display bench */
    int                 outfd;          /* fd of file receving program output, -1 if stdout or stderr */
} ctx_t;

static int clean_ctx(int ret, ctx_t * ctx) {
    if (ctx) {
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
    }
    return ret;
}

static void header(FILE * out) {
    fprintf(out, "%s v%s %s built on %s, %s from git:%s\n\n",
#           ifdef HAVE_VERSION_H
            BUILD_APPNAME, APP_VERSION, BUILD_APPRELEASE, __DATE__, __TIME__, BUILD_GITREV
#           else
            "vrunas", "?", __DATE__, __TIME__, "?"
#           endif
            );
    fprintf(out, "Copyright (C) 2018 Vincent Sallaberry.\n"
            "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n" \
            "This is free software: you are free to change and redistribute it.\n" \
            "There is NO WARRANTY, to the extent permitted by law.\n\n");
}

static int usage(int ret, ctx_t * ctx) {
    FILE * out = ret ? stderr : stdout;

    header(out);
    fprintf(out, "Usage: %s [-h] [-u uid|user] [-g gid|group] [-U user] [-G group] [-t|-T] [-1|-2] [-o|-O file]"
#                             ifdef APP_INCLUDE_SOURCE
                              " [-s]"
#                             endif
#                             ifdef _TEST
                              /* nothing */
#                             endif
                              " [program [arguments]]\n"
#           ifdef APP_INCLUDE_SOURCE
            "  -s           : show source\n"
#           endif
            "  -u uid|user  : change uid\n"
            "  -g gid|group : change gid\n"
            "  -U user      : print uid of user, no program arguments needed.\n"
            "  -G group     : print gid of group, no program arguments needed.\n"
            "  -1|-2        : redirect program stderr or stdout to respectively stdout(-1) or stderr(-2)\n"
            "  -t|-T        : print timings of program (-t:'time -p' posix format, -T:extended)\n"
            "                 with -1, timings will be printed to err, with -2, to out, otherwise, to err.\n"
            "  -o|-O file   : redirect program out to file (-O:append).\n"
            "                 With -1 or -2, program err and out are redirected to file.\n"
#           ifdef _TEST
            /* nothing */
#           endif
            "  -h           : help\n"
            "\n", (ctx && ctx->argv ? *ctx->argv : "vrunas"));
    return clean_ctx(ret, ctx);
}

static int nam2id_alloc_r(char ** pbuf, size_t * pbufsz) {
    if (pbuf == NULL || pbufsz == NULL)
        return -1;
    if (*pbuf == NULL) {
        static const int    confs[] = { _SC_GETPW_R_SIZE_MAX, _SC_GETGR_R_SIZE_MAX };
        long                size = 0, ret;
        for (size_t i = 0; i < sizeof(confs) / sizeof(*confs); i++) {
            if ((ret = sysconf(confs[i])) > size)
                size = ret;
        }
        *pbufsz = (size > 0 ? size : 16384);
        if ((*pbuf = malloc(*pbufsz)) == NULL)
            fprintf(stderr, "nam2id_alloc(malloc): %s\n", strerror(errno));
    }
    return *pbuf ? 0 : -1;
}

/**
 * pwnamid_r(): wrapper to getpwnam_r() with automatic memory allocation.
 * @param str the user_name to look for
 * @param uid the resulting uid
 * @param pbuf the pointer to buffer used by getpwnam_r. if NULL it is malloced and
 *             freed, if not null and allocated, it is used, if not null and not
 *             allocated, it is malloced. When pbuf not null, the caller must free *pbuf.
 * @param pbufsz the pointer to size of *pbuf
 * @return 0 on success (str, *uid, *pbuf and *pbufsz usable, -1 otherwise)
 */
static int pwnam2id_r(const char * str, uid_t *uid, char ** pbuf, size_t * pbufsz) {
    struct passwd       pwd;
    struct passwd *     pwdres;
    char *              buf = NULL;
    size_t              bufsz;
    int                 ret = -1;

    if (!pbuf)      pbuf    = &buf;
    if (!pbufsz)    pbufsz  = &bufsz;
    if (nam2id_alloc_r(pbuf, pbufsz) != 0) {
        return -1;
    }

    if (((str == NULL || uid == NULL) && (errno = EFAULT))
    ||  getpwnam_r(str, &pwd, *pbuf, *pbufsz, &pwdres) != 0
    ||  (pwdres == NULL && (errno = EINVAL))) {
        fprintf(stderr, "user `%s` (getpwnam_r): %s\n", str, strerror(errno));
    } else {
        ret = errno = 0;
        *uid = pwdres->pw_uid;
    }
    if (pbuf == &buf)
        free(*pbuf);
    return ret;
}

/** grnam2id_r(): wrapper to getgrnam_r() with automatic memory allocation.
 * See pwnam2id_r() */
static int grnam2id_r(const char * str, gid_t *gid, char ** pbuf, size_t * pbufsz) {
    struct group        pwd;
    struct group *      pwdres;
    char *              buf = NULL;
    size_t              bufsz;
    int                 ret = -1;

    if (!pbuf)      pbuf    = &buf;
    if (!pbufsz)    pbufsz  = &bufsz;
    if (nam2id_alloc_r(pbuf, pbufsz) != 0) {
        return -1;
    }

    if (((str == NULL || gid == NULL) && (errno = EFAULT))
    ||  getgrnam_r(str, &pwd, *pbuf, *pbufsz, &pwdres) != 0
    ||  (pwdres == NULL && (errno = EINVAL))) {
        fprintf(stderr, "group `%s` (getgrnam_r): %s\n", str, strerror(errno));
    } else {
        ret = errno = 0;
        *gid = pwdres->gr_gid;
    }
    if (pbuf == &buf)
        free(*pbuf);
    return ret;
}

int set_uidgid(uid_t uid, gid_t gid, ctx_t * ctx) {
    /* set gid if given */
    if ((ctx->flags & HAVE_GID) != 0) {
        if (setgid(gid) < 0) {
            fprintf(stderr, "`%lu` (setgid): %s\n", (unsigned long) gid, strerror(errno));
            return ERR_SETID;
        } else fprintf(stderr, "setting gid to %u\n", gid);
    }
    /* set uid if given */
    if ((ctx->flags & HAVE_UID) != 0) {
        if (setuid(uid) < 0) {
            fprintf(stderr, "`%lu` (setuid): %s\n", (unsigned long) uid, strerror(errno));
            return ERR_SETID;
        } else fprintf(stderr, "setting uid to %u\n", uid);
    }
    return 0;
}

char ** build_argv(int argc, char * const * argv, ctx_t * ctx) {
    char ** newargv, ** tmp;
    (void)ctx;

    if ((tmp = newargv = malloc(argc + 1)) == NULL) {
        fprintf(stderr, "build_argv(malloc) : %s\n", strerror(errno));
        return NULL;
    }
    while (argc--)  {
        *(tmp++) = *(argv++);
    }
    *tmp = NULL;
    return newargv;
}

int set_redirections(ctx_t * ctx) {
    int backupfd;
    int dupfd;
    int redirectedfd = -1;

    /* If bench is ON, we want it alone on its output so as it is easy for shell
     * scripts to catch (ctx->alternate file) */
    /* With '-2', stdout is redirected to stderr. If bench is ON,
     * it is displayed on the real stdout (ctx->alternatefile) */
    if ((ctx->flags & TO_STDERR) != 0) {
        dupfd = STDERR_FILENO;
        redirectedfd = STDOUT_FILENO;
    }
    /* Else, with '-1' or if bench is ON, stderr is redirected to stdout, and
     * bench is displayed on the real stderr (ctx->alternatefile) */
    else if ((ctx->flags & TO_STDOUT) != 0  || (ctx->flags & (TIME | TIMEEXT)) != 0) {
        dupfd = STDOUT_FILENO;
        redirectedfd = STDERR_FILENO;
    }
    if (redirectedfd >= 0) {
        /* make a backup of stderr */
        if ((backupfd = dup(redirectedfd)) < 0) {
            /* error TODO */
            return -1;
        }
        if ((ctx->alternatefile = fdopen(backupfd, "w")) == NULL) {
            /* error TODO */
            return -1;
        }
        /* redirect stderr on stdout */
        if (dup2(dupfd, redirectedfd) < 0) {
            /* error TODO */
            return -1;
        }
    }
    return 0;
}

int set_out(const char * file, ctx_t * ctx) {
    int open_flags = O_WRONLY | O_CREAT;
    int fd;

    if (file == NULL)
        return 0;

    if ((ctx->flags & OUT_APPEND) != 0)
        open_flags |= O_APPEND;
    else
        open_flags |= O_TRUNC;

    if ((fd = open(file, open_flags)) < 0) {
        /* error */
        fprintf(stderr, "set_out(open), %s: %s\n", file, strerror(errno));
        return -1;
    }

    /* only stdout is dupped to file. Enabling option -1, will include stderr */
    /* FIXME : bug with option -2 */
    if (dup2(STDOUT_FILENO, fd) < 0) {
        fprintf(stderr, "set_out(dup2): %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int do_bench(ctx_t * ctx) {
    if ((ctx->flags & (TIME | TIMEEXT)) != 0) {
        pid_t wpid, pid = fork();
        struct tms tms;
        clock_t t0 = clock();
#if 0
        struct timespec ts
#endif
        if (pid < 0) {
            perror("fork");
            return ERR_BENCH;
        }
        if (pid == 0) {
            /* son */
            //yield();
            return 0;
        } else {
            struct rusage rusage;
            FILE * out = ctx->alternatefile;
            int status;

            /* father */
            //wait4(pid, &status, 0 /*options*/, &rusage);
            if ((wpid = waitpid(pid, &status, 0 /* options */)) <= 0)
                perror("waitpid");
            if (times(&tms) == (clock_t) -1)
                perror("times");
            if (getrusage(RUSAGE_CHILDREN, &rusage) < 0)
                perror("getrusage");
            if ((ctx->flags & TIME) != 0) {
                fprintf(out, "real %ld.%02d\nuser %ld.%02d\nsys %ld.%02d\n",
                        0L, 0,
                        rusage.ru_utime.tv_sec, rusage.ru_utime.tv_usec / 10000,
                        rusage.ru_stime.tv_sec, rusage.ru_stime.tv_usec / 10000);
            }
            if ((ctx->flags & TIMEEXT) != 0) {
                fprintf(out, "ncvsw %ld\nnivcsw %ld\nisrss %ld\nidrss %ld\nixrss %ld\nmaxrss %ld\n"
                             "msgsnd %ld\nmsgrcv %ld\n"
                             "inblock %ld\noutblock %ld\n"
                             "first %ld\n"
                             "minflt %ld\nmaxflt %ld\n"
                             "nswps %ld\nnsigs %ld\n"
                        ,rusage.ru_nvcsw, rusage.ru_nivcsw,
                        rusage.ru_isrss, rusage.ru_idrss, rusage.ru_ixrss, rusage.ru_maxrss,
                        rusage.ru_msgsnd, rusage.ru_msgrcv,
                        rusage.ru_inblock, rusage.ru_oublock,
                        rusage.ru_first,
                        rusage.ru_minflt, rusage.ru_majflt,
                        rusage.ru_nswap,
                        rusage.ru_nsignals
                       );
            }
            clean_ctx(0, ctx);
            // TODO
            if (WIFEXITED(status)) {
                exit(WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                exit(-100-WTERMSIG(status));
            }
        }
    }
    return 0;
}

int main(int argc, char *const* argv) {
    ctx_t           ctx = { .flags = 0, .argc = argc, .argv = argv, .buf = NULL, .bufsz = 0, .alternatefile = NULL, .outfd = -1 };
    char **         newargv = NULL;
    const char *    outfile = NULL;
    uid_t           uid = 0;
    gid_t           gid = 0;
    int             i_argv;
    int             ret = 0;

    for (i_argv = 1; i_argv < argc; i_argv++) {
        if (*argv[i_argv] == '-') {
            for (const char * arg = argv[i_argv] + 1; *arg; arg++) {
                switch (*arg) {
                    char *  endptr = NULL;
                    uid_t   tmpuid;
                    gid_t   tmpgid;
                    case 't': ctx.flags |= TIME;        break ;
                    case 'T': ctx.flags |= TIMEEXT;     break ;
                    case '1':
                        if ((ctx.flags & TO_STDERR) != 0)
                            fprintf(stderr, "warning, overiding previous '-2' with option '-1'\n");
                        ctx.flags = (ctx.flags & ~TO_STDERR) | TO_STDOUT;
                        break ;
                    case '2':
                        if ((ctx.flags & TO_STDOUT) != 0)
                            fprintf(stderr, "warning, overiding previous '-1' option with '-2'\n");
                        ctx.flags = (ctx.flags & ~TO_STDOUT) | TO_STDERR;
                        break ;
                    case 'o':
                    case 'O':
                        if (++i_argv >= argc || arg[1])
                            return usage(ERR_OPTION+9, &ctx);
                        if (outfile != NULL)
                            fprintf(stderr, "warning, overrinding previous '-%c %s' with '-%c %s'\n",
                                    (ctx.flags & OUT_APPEND) != 0 ? 'O' : 'o', outfile, *arg, argv[i_argv]);
                        if (*arg == 'O')
                            ctx.flags |= OUT_APPEND;
                        else
                            ctx.flags &= ~OUT_APPEND;
                        outfile = argv[i_argv];
                        break ;
                    case 'u':
                        if (++i_argv >= argc || arg[1])
                            return usage(ERR_OPTION+8, &ctx);
                        if ((ctx.flags & HAVE_UID) != 0)
                            fprintf(stderr, "warning, overriding previous `-u` parameter with new value `%s`\n", argv[i_argv]);
                        errno = 0;
                        tmpuid = strtol(argv[i_argv], &endptr, 0);
                        if ((errno != 0 || !endptr || *endptr != 0)
                        && pwnam2id_r(argv[i_argv], &tmpuid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(ERR_OPTION+7, &ctx);
                        ctx.flags |= HAVE_UID;
                        uid = tmpuid;
                        break ;
                    case 'U':
                        if (++i_argv >= argc || arg[1])
                            return usage(ERR_OPTION+6, &ctx);
                        if (pwnam2id_r(argv[i_argv], &tmpuid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(ERR_OPTION+5, &ctx);
                        ctx.flags |= OPTIONAL_ARGS;
                        fprintf(stdout, "%lu\n", (unsigned long) tmpuid);
                        break ;
                    case 'g':
                        if (++i_argv >= argc || arg[1])
                            return usage(ERR_OPTION+4, &ctx);
                        if ((ctx.flags & HAVE_GID) != 0)
                            fprintf(stderr, "warning, overriding previous `-g` parameter with new value `%s`\n", argv[i_argv]);
                        errno = 0;
                        tmpgid = strtol(argv[i_argv], &endptr, 0);
                        if ((errno != 0 || !endptr || *endptr != 0)
                        && grnam2id_r(argv[i_argv], &tmpgid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(ERR_OPTION+3, &ctx);
                        ctx.flags |= HAVE_GID;
                        gid = tmpgid;
                        break ;
                    case 'G':
                        if (++i_argv >= argc || arg[1])
                            return usage(ERR_OPTION+2, &ctx);
                        if (grnam2id_r(argv[i_argv], &tmpgid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(ERR_OPTION+1, &ctx);
                        ctx.flags |= OPTIONAL_ARGS;
                        fprintf(stdout, "%lu\n", (unsigned long) tmpgid);
                        break ;
#                   ifdef APP_INCLUDE_SOURCE
                    case 's':
                        for (const char *const* line = vrunas_get_source(); *line; line++)
                            fprintf(stdout, "%s", *line);
                        break ;
#                   endif
#                   ifdef _TEST
                    case 'd': break ;
#                   endif
                    case 'h': return usage(0, &ctx);
                    default:  return usage(ERR_OPTION, &ctx);
                }
            }
        } else break ;
    }
    /* clean now unnecessary resources */
    if (ctx.buf) {
        free(ctx.buf);
        ctx.buf = NULL;
    }
    do {
        /* first of all, set up file redirections */
        if (set_redirections(&ctx) != 0 && (ret = ERR_REDIR))
            break ;
        /* error if program is mandatory */
        if (i_argv >= argc) {
            if ((ctx.flags & OPTIONAL_ARGS) != 0 && ((ret = 0) || 1))
                break ;
            fprintf(stderr, "error: missing program\n");
            ret = usage(ERR_PROG_MISSING, &ctx);
            break ;
        }
        /* program header */
        header(stdout);
        /* prepare uid, gid, newargv, outfile, bench for excvp */
        if (set_uidgid(uid, gid, &ctx) != 0 && (ret = ERR_SETID))
            break ;
        if (set_out(outfile, &ctx) != 0 && (ret = ERR_SETOUT))
            break ;
        if (do_bench(&ctx) != 0 && (ret = ERR_BENCH))
            break ;
        if ((newargv = build_argv(argc - i_argv, argv + i_argv, &ctx)) == NULL && (ret = ERR_BUILDARGV))
            break ;
        /* execvp, in, if needed, a forked process */
        if (execvp(*newargv, newargv) < 0) {
            ret = ERR_EXEC;
            fprintf(stderr, "`%s` (", *newargv); perror("execvp)");
            break ;
        }
        /* not reachable */
        return ERR_NOT_REACHABLE;
    } while (0);
    if (newargv)
        free(newargv);
    return clean_ctx(ret, &ctx);
}


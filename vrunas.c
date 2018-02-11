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
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef HAVE_VERSION_H
# include "version.h"
#endif
#ifdef APP_INCLUDE_SOURCE
const char *const* vrunas_get_source();
#endif

typedef struct {
    int                 argc;
    char *const*        argv;
    char *              buf;
    size_t              bufsz;
} ctx_t;

static int clean_ctx(int ret, ctx_t * ctx) {
    if (ctx && ctx->buf) {
        free(ctx->buf);
        ctx->buf = NULL;
    }
    return ret;
}

static int usage(int ret, ctx_t * ctx) {
    FILE * out = ret ? stderr : stdout;
    fprintf(out, "Usage: %s [-h] [-u uid|user] [-g gid|group] [-U user] [-G group]"
#                             ifdef APP_INCLUDE_SOURCE
                              " [-s]"
#                             endif
#                             ifdef _TEST
                              " [-T]"
#                             endif
                              " [program [arguments]]\n"
#           ifdef APP_INCLUDE_SOURCE
            "  -s           : show source\n"
#           endif
            "  -u uid|user  : change uid\n"
            "  -g gid|group : change gid\n"
            "  -U user      : print uid of group, no program arguments needed.\n"
            "  -G group     : print gid of group, no program arguments needed.\n"
#           ifdef _TEST
            "  -T           : run tests\n"
#           endif
            "  -h           : help\n"
            "\n", (ctx && ctx->argv ? *ctx->argv : "vrunas"));
    return clean_ctx(ret, ctx);
}

#define HAVE_UID        1 << 0
#define HAVE_GID        1 << 1
#define OPTIONAL_ARGS   1 << 2

static int nam2id_alloc_r(char ** pbuf, size_t * pbufsz) {
    if (pbuf == NULL || pbufsz == NULL)
        return -1;
    if (*pbuf == NULL) {
        static const int    confs[] = { _SC_GETPW_R_SIZE_MAX, _SC_GETGR_R_SIZE_MAX };
        int                 size = 0, ret;
        for (size_t i = 0; i < sizeof(confs); i++) {
            if ((ret = sysconf(confs[i])) > size)
                size = ret;
        }
        *pbufsz = (size > 0 ? size : 16384);
        *pbuf = malloc(*pbufsz);
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
        fprintf(stderr, "user `%s` (", str); perror("getpwnam_r)");
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
        fprintf(stderr, "group `%s` (", str); perror("getgrnam_r)");
    } else {
        ret = errno = 0;
        *gid = pwdres->gr_gid;
    }
    if (pbuf == &buf)
        free(*pbuf);
    return ret;
}

int main(int argc, char *const* argv) {
    ctx_t           ctx = { argc, argv, NULL, 0 };
    char **         newargv;
    uid_t           uid = 0;
    gid_t           gid = 0;
    int             flags = 0;
    int             i_argv;
    int             i;

    fprintf(stderr, "%s v%s built on %s, %s from git-rev %s\n",
#           ifdef HAVE_VERSION_H
            BUILD_APPNAME, APP_VERSION, __DATE__, __TIME__, BUILD_GITREV
#           else
            "vrunas", "?", __DATE__, __TIME__, "?"
#           endif
            );
    fprintf(stderr, "Copyright (C) 2018 Vincent Sallaberry.\n"
            "This is free software; see the source for copying conditions.  There is NO\n"
            "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n");

    for (i_argv = 1; i_argv < argc; i_argv++) {
        if (*argv[i_argv] == '-') {
            for (const char * arg = argv[i_argv] + 1; *arg; arg++) {
                switch (*arg) {
                    char *  endptr = NULL;
                    uid_t   tmpuid;
                    gid_t   tmpgid;
                    case 'u':
                        if (++i_argv >= argc || arg[1])
                            return usage(18, &ctx);
                        if ((flags & HAVE_UID) != 0)
                            fprintf(stderr, "warning, overriding previous `-u` parameter with new value `%s`\n", argv[i_argv]);
                        errno = 0;
                        tmpuid = strtol(argv[i_argv], &endptr, 0);
                        if ((errno != 0 || !endptr || *endptr != 0)
                        && pwnam2id_r(argv[i_argv], &tmpuid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(17, &ctx);
                        flags |= HAVE_UID;
                        uid = tmpuid;
                        break ;
                    case 'U':
                        if (++i_argv >= argc || arg[1])
                            return usage(16, &ctx);
                        if (pwnam2id_r(argv[i_argv], &tmpuid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(15, &ctx);
                        flags |= OPTIONAL_ARGS;
                        fprintf(stdout, "%lu\n", (unsigned long) tmpuid);
                        break ;
                    case 'g':
                        if (++i_argv >= argc || arg[1])
                            return usage(14, &ctx);
                        if ((flags & HAVE_GID) != 0)
                            fprintf(stderr, "warning, overriding previous `-g` parameter with new value `%s`\n", argv[i_argv]);
                        errno = 0;
                        tmpgid = strtol(argv[i_argv], &endptr, 0);
                        if ((errno != 0 || !endptr || *endptr != 0)
                        && grnam2id_r(argv[i_argv], &tmpgid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(13, &ctx);
                        flags |= HAVE_GID;
                        gid = tmpgid;
                        break ;
                    case 'G':
                        if (++i_argv >= argc || arg[1])
                            return usage(12, &ctx);
                        if (grnam2id_r(argv[i_argv], &tmpgid, &ctx.buf, &ctx.bufsz) != 0)
                            return clean_ctx(11, &ctx);
                        flags |= OPTIONAL_ARGS;
                        fprintf(stdout, "%lu\n", (unsigned long) tmpgid);
                        break ;
#                   ifdef APP_INCLUDE_SOURCE
                    case 's':
                        for (const char *const* line = vrunas_get_source(); *line; line++)
                            fprintf(stdout, "%s", *line);
                        break ;
#                   endif
#                   ifdef _TEST
                    case 'T': break ;
#                   endif
                    case 'h': return usage(0, &ctx);
                    default:  return usage(10, &ctx);
                }
            }
        } else break ;
    }
    /* free resources */
    clean_ctx(0, &ctx);
    /* error if program is mandatory */
    if (i_argv >= argc) {
        if ((flags & OPTIONAL_ARGS) != 0)
           return 0;
        fprintf(stderr, "error: missing program\n");
        return usage(1, &ctx);
    }
    /* set gid if given */
    if ((flags & HAVE_GID) != 0) {
        if (setgid(gid) < 0) {
            fprintf(stderr, "`%lu` (", (unsigned long) gid); perror("setgid)");
            return 2;
        } else fprintf(stderr, "setting gid to %u\n", gid);
    }
    /* set uid if given */
    if ((flags & HAVE_UID) != 0) {
        if (setuid(uid) < 0) {
            fprintf(stderr, "`%lu` (", (unsigned long) uid); perror("setuid)");
            return 3;
        } else fprintf(stderr, "setting uid to %u\n", uid);
    }
    /* prepare newargv for excvp */
    if ((newargv = malloc(argc - i_argv + 1)) == NULL) {
        perror("malloc argv for execvp");
        return 4;
    }
    for (i = 0 ; i_argv < argc; i_argv++, i++) {
        newargv[i] = argv[i_argv];
    }
    newargv[i] = NULL;
    /* execvp */
    if (execvp(*newargv, newargv) < 0) {
        free(newargv);
        fprintf(stderr, "`%s` (", *newargv); perror("execvp)");
        return 5;
    }
    /* not reachable */
    return -1;
}


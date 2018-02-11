/*
 * Copyright (C) 2018 Vincent Sallaberry
 * vrunas
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

static int usage(int ret, int argc, char *const* argv) {
    FILE * out = ret ? stderr : stdout;
    fprintf(out, "\nUsage: %s [-u uid|user] [-g gid|group] [-U user] [-G group]"
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
            "\n", *argv);
    (void)argc;
    return ret;
}

#define HAVE_UID        1 << 0
#define HAVE_GID        1 << 1
#define OPTIONAL_ARGS   1 << 2

static int nam2id_alloc_r(char ** pbuf, size_t * pbufsz) {
    if (pbuf == NULL || pbufsz == NULL)
        return -1;
    if (*pbuf == NULL) {
        *pbufsz = 16384;
        *pbuf = malloc(*pbufsz);
    }
    return *pbuf ? 0 : -1;
}

/**
 * pwnamid_r(): wrapper to getpwnam_r() with automatic memory allocation.
 * @param str the user_name to look for
 * @param uid the resulting uid
 * @param pbuf the pointer to buffer used by getpwnam_r. if NULL it is malloced and
 *             freed, if not null and allocated it is used, if not null and not
 *             allocated it is malloced. When pbuf not null, the caller must free it.
 * @param pbufsz the pointer to size of *puf
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

    if (((str == NULL || uid == NULL) && (errno = EINVAL))
    ||  getpwnam_r(str, &pwd, *pbuf, *pbufsz, &pwdres) != 0
    ||  (pwdres == NULL && (errno = ENOENT))) {
        fprintf(stderr, "`%s` (", str); perror("getpwnam_r)");
    } else {
        ret = errno = 0;
        *uid = pwd.pw_uid;
    }
    if (pbuf == &buf)
        free(*pbuf);
    return ret;
}

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

    if (((str == NULL || gid == NULL) && (errno = EINVAL))
    ||  getgrnam_r(str, &pwd, *pbuf, *pbufsz, &pwdres) != 0
    ||  (pwdres == NULL && (errno = ENOENT))) {
        fprintf(stderr, "`%s` (", str); perror("getgrnam_r)");
    } else {
        ret = errno = 0;
        *gid = pwd.gr_gid;
    }
    if (pbuf == &buf)
        free(*pbuf);
    return ret;
}

int main(int argc, char *const* argv) {
    char **         newargv;
    char *          buf = NULL;
    size_t          bufsz;
    uid_t           uid = 0;
    gid_t           gid = 0;
    int             flags = 0;
    int             i_argv;
    int             i;

    for (i_argv = 1; i_argv < argc; i_argv++) {
        if (*argv[i_argv] == '-') {
            for (const char * arg = argv[i_argv] + 1; *arg; arg++) {
                switch (*arg) {
                    char *  endptr = NULL;
                    uid_t   tmpuid;
                    gid_t   tmpgid;
                    case 'u':
                    case 'U':
                        if (++i_argv >= argc || arg[1])
                            return usage(14, argc, argv);
                        errno = 0;
                        if (*arg == 'u')
                            tmpuid = strtol(argv[i_argv], &endptr, 0);
                        if (((!endptr || *endptr != 0) && pwnam2id_r(argv[i_argv], &tmpuid, NULL, NULL) != 0) || errno != 0)
                            return usage(13, argc, argv);
                        if (*arg == 'u') {
                            flags |= HAVE_UID;
                            uid = tmpuid;
                        } else {
                            flags |= OPTIONAL_ARGS;
                            fprintf(stdout, "%lu\n", (unsigned long) tmpuid);
                        }
                        break ;
                    case 'g':
                    case 'G':
                        if (++i_argv >= argc || arg[1])
                            return usage(12, argc, argv);
                        errno = 0;
                        if (*arg == 'g')
                            tmpgid = strtol(argv[i_argv], &endptr, 0);
                        if (((!endptr || *endptr != 0) && grnam2id_r(argv[i_argv], &tmpgid, NULL, NULL) != 0) || errno != 0)
                            return usage(11, argc, argv);
                        if (*arg == 'g') {
                            flags |= HAVE_GID;
                            gid = tmpgid;
                        } else {
                            flags |= OPTIONAL_ARGS;
                            fprintf(stdout, "%lu\n", (unsigned long) tmpgid);
                        }
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
                    case 'h': return usage(0, argc, argv);
                    default:  return usage(10, argc, argv);
                }
            }
        } else break ;
    }
    /* free resources */
    if (buf)
        free(buf);
    /* error if program is mandatory */
    if (i_argv >= argc) {
        if ((flags & OPTIONAL_ARGS) != 0)
           return 0;
        fprintf(stderr, "error: missing program\n");
        return 1;
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


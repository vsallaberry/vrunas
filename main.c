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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

static int usage(int ret, int argc, char *const* argv) {
    fprintf(ret ? stderr : stdout, "\nUsage: %s [-u uid] [-g gid] [-s] [-T] program arguments\n"
#           ifdef APP_INCLUDE_SOURCE
            "  -s     : show source\n"
#           endif
            "  -u uid : change uid\n"
            "  -g gid : change gid\n"
#           ifdef _TEST
            "  -T     : run tests\n"
#           endif
            "\n", *argv);
    (void)argc;
    return ret;
}

#define HAVE_UID    1 << 0
#define HAVE_GID    1 << 1

int main(int argc, char *const* argv) {
    char **         newargv;
    uid_t           uid;
    gid_t           gid;
    int             flags = 0;
    int             i_argv;
    int             i;

    for (i_argv = 1; i_argv < argc; i_argv++) {
        if (*argv[i_argv] == '-') {
            for (const char * arg = argv[i_argv] + 1; *arg; arg++) {
                switch (*arg) {
                    case 'u':
                        if (i_argv + 1 >= argc || arg[1])
                            return usage(14, argc, argv);
                        errno = 0;
                        uid = strtol(argv[++i_argv], NULL, 0);
                        if (errno != 0)
                            return usage(13, argc, argv);
                        flags |= HAVE_UID;
                        break ;
                    case 'g':
                        if (i_argv + 1 >= argc || arg[1])
                            return usage(12, argc, argv);
                        errno = 0;
                        gid = strtol(argv[++i_argv], NULL, 0);
                        if (errno != 0)
                            return usage(11, argc, argv);
                        flags |= HAVE_GID;
                        break ;
#                   ifdef APP_INCLUDE_SOURCE
                    case 's':
                        for (const char *const* line = vrunas_get_source(); *line; line++)
                            fprintf(stdout, "%s", *line);
                        break ;
#                   endif
                    case 'h': return usage(0, argc, argv);
                    default:  return usage(10, argc, argv);
                }
            }
        } else break ;
    }
    if (i_argv >= argc) {
        fprintf(stderr, "error: missing program\n");
        exit(1);
    }
    if ((flags & HAVE_GID) != 0) {
        if (setgid(gid) < 0) {
            perror("setgid()");
            exit(2);
        } else fprintf(stderr, "setting gid to %u\n", gid);
    }
    if ((flags & HAVE_UID) != 0) {
        if (setuid(uid) < 0) {
            perror("setuid()");
            exit(3);
        } else fprintf(stderr, "setting uid to %u\n", uid);
    }
    if ((newargv = malloc(argc - i_argv + 1)) == NULL) {
        perror("malloc");
        exit(4);
    }
    for (i = 0 ; i_argv < argc; i_argv++, i++) {
        newargv[i] = argv[i_argv];
    }
    newargv[i] = NULL;
    if (execvp(*newargv, newargv) < 0) {
        perror("execvp");
        exit(5);
    }
    /* not reachable */
    return -1;
}


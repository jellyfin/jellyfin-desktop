/*
 * Statically-linked wrapper for QtWebEngineProcess.
 *
 * Qt WebEngine launches this as a subprocess.  Because the parent sets
 * LD_LIBRARY_PATH to the AppImage's bundled libraries, a shell-script
 * wrapper would crash: the host's /bin/bash gets loaded by the host's
 * dynamic linker, which then pulls in the *bundled* glibc — an ABI
 * mismatch that segfaults on distros whose glibc differs from the build
 * host (e.g. Ubuntu vs Arch).
 *
 * A static binary sidesteps the problem entirely: it has no shared
 * library dependencies, so LD_LIBRARY_PATH is irrelevant.  It simply
 * re-execs the real QtWebEngineProcess binary through the bundled
 * ld-linux with an explicit --library-path.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    char self[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len < 0)
        _exit(127);
    self[len] = '\0';

    /* Derive APPDIR: this binary lives under $APPDIR/usr/lib/...,
       so the first /usr/lib/ in the path marks the boundary. */
    char *p = strstr(self, "/usr/lib/");
    if (!p)
        _exit(127);

    int appdir_len = (int)(p - self);

    char ldlinux[PATH_MAX];
    snprintf(ldlinux, sizeof(ldlinux),
             "%.*s/usr/lib/ld-linux-x86-64.so.2", appdir_len, self);

    char real_bin[PATH_MAX + 8];
    snprintf(real_bin, sizeof(real_bin), "%s.real", self);

    char lib_path[PATH_MAX * 2];
    snprintf(lib_path, sizeof(lib_path),
             "%.*s/usr/lib"
             ":/usr/lib64:/usr/lib:/lib64:/lib"
             ":/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu",
             appdir_len, self);

    /* argv: ld-linux --inhibit-cache --library-path <path> <real> [original args…] */
    char **new_argv = calloc((size_t)(argc + 5), sizeof(char *));
    if (!new_argv)
        _exit(127);

    new_argv[0] = ldlinux;
    new_argv[1] = "--inhibit-cache";
    new_argv[2] = "--library-path";
    new_argv[3] = lib_path;
    new_argv[4] = real_bin;
    for (int i = 1; i < argc; i++)
        new_argv[i + 4] = argv[i];

    execv(ldlinux, new_argv);
    _exit(127);
}

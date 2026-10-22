/* POSIX shared memory for bionic (API 24 has no shm_open): the named region is a file in /dev, which is a tmpfs on the Echo.  Included into nqptp and
 * shairport-sync with -include (tools/airplay/build.sh), so neither source is patched.  nqptp (root) makes /dev/nqptp... with the process's umask (0644),
 * shairport-sync only maps it read-only. */
#ifndef BIONIC_SHM_H
#define BIONIC_SHM_H
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
static inline int hm_shm_open(const char *name, int flags, mode_t mode)
{
    char p[160];
    snprintf(p, sizeof p, "/dev/%s", name + (name[0] == '/'));
    return open(p, flags | O_CLOEXEC, mode);
}
static inline int hm_shm_unlink(const char *name)
{
    char p[160];
    snprintf(p, sizeof p, "/dev/%s", name + (name[0] == '/'));
    return unlink(p);
}
#define shm_open hm_shm_open
#define shm_unlink hm_shm_unlink
#endif

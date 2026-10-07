/* Darwin's extended getgroups reports account defaults, not process groups.
 * Select ordinary getgroups before any includes; default Darwin FULL still
 * exposes setgroups (do not request a restricted POSIX namespace). */
#undef _DARWIN_C_SOURCE
#undef _DARWIN_UNLIMITED_GETGROUPS

#include <sys/types.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ethernet_raw_privilege.h"

static bool identity(const char *text, uint32_t *value)
{
    if (text == NULL || *text == '\0') { return false; }
    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9') { return false; }
    }
    errno = 0;
    char *end;
    const unsigned long n = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || n == 0u || n >= UINT32_MAX)
    {
        return false;
    }
    *value = (uint32_t)n;
    return true;
}

static bool failed(const char *operation, int error)
{
    fprintf(stderr, "Privilege drop: %s: %s\n", operation, strerror(error));
    return false;
}

bool raw_drop_privileges(void)
{
    if (geteuid() != 0) { return true; }
    uint32_t uid, gid;
    if (!identity(getenv("SUDO_UID"), &uid) ||
        !identity(getenv("SUDO_GID"), &gid))
    {
        fprintf(stderr, "Root invocation needs nonzero SUDO_UID/SUDO_GID; "
                "refusing a packet loop as root\n");
        return false;
    }
    if (setgroups(0, NULL) != 0)
    {
        const int error = errno;
        return failed("setgroups(0)", error);
    }
    if (setgid((gid_t)gid) != 0)
    {
        const int error = errno;
        return failed("setgid", error);
    }
    if (setuid((uid_t)uid) != 0)
    {
        const int error = errno;
        return failed("setuid", error);
    }
    if (getuid() != (uid_t)uid)
    {
        fprintf(stderr, "Privilege drop: real UID mismatch\n");
        return false;
    }
    if (geteuid() != (uid_t)uid)
    {
        fprintf(stderr, "Privilege drop: effective UID mismatch\n");
        return false;
    }
    if (getgid() != (gid_t)gid)
    {
        fprintf(stderr, "Privilege drop: real GID mismatch\n");
        return false;
    }
    if (getegid() != (gid_t)gid)
    {
        fprintf(stderr, "Privilege drop: effective GID mismatch\n");
        return false;
    }
    const int groups = getgroups(0, NULL);
    if (groups < 0)
    {
        const int error = errno;
        return failed("getgroups(0)", error);
    }
    /* XNU's ordinary getgroups includes the effective GID at groups[0]. */
    if (groups != 1)
    {
        fprintf(stderr, "Privilege drop: getgroups(0) expected one effective "
                "GID, returned %d\n", groups);
        return false;
    }
    gid_t actual_gid;
    const int fetched = getgroups(1, &actual_gid);
    if (fetched < 0)
    {
        const int error = errno;
        return failed("getgroups(1)", error);
    }
    if (fetched != 1)
    {
        fprintf(stderr, "Privilege drop: getgroups(1) expected one effective "
                "GID, returned %d\n", fetched);
        return false;
    }
    if (actual_gid != (gid_t)gid)
    {
        fprintf(stderr, "Privilege drop: getgroups(1) effective GID mismatch\n");
        return false;
    }
    errno = 0;
    const int regain = setuid(0);
    const int regain_error = regain == -1 ? errno : 0;
    if (regain != -1)
    {
        fprintf(stderr, "Privilege drop: setuid(0) unexpectedly returned %d\n",
                regain);
        return false;
    }
    if (regain_error != EPERM)
    {
        return failed("setuid(0) expected EPERM", regain_error);
    }
    return true;
}

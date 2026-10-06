/* Offline only: every identity/group syscall is replaced before inclusion.
 * Match the implementation's namespace before any system header is parsed. */
#undef _DARWIN_C_SOURCE
#undef _DARWIN_UNLIMITED_GETGROUPS
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char calls[64];
static size_t count;
static char failure;
static const char *uid_text;
static const char *gid_text;
static uid_t real_uid, effective_uid;
static gid_t real_gid, effective_gid;
static int group_count, fetch_count, regain_result, regain_errno;
static gid_t fetched_gid;

static void call(char operation)
{
    assert(count + 1u < sizeof calls);
    calls[count++] = operation;
    calls[count] = '\0';
}

static char *mock_getenv(const char *name)
{
    if (strcmp(name, "SUDO_UID") == 0)
    {
        call('a');
        return (char *)uid_text;
    }
    assert(strcmp(name, "SUDO_GID") == 0);
    call('b');
    return (char *)gid_text;
}

static uid_t mock_geteuid(void)
{
    call('e');
    return effective_uid;
}

static uid_t mock_getuid(void) { call('U'); return real_uid; }
static gid_t mock_getgid(void) { call('G'); return real_gid; }
static gid_t mock_getegid(void) { call('g'); return effective_gid; }

static int setter(char operation)
{
    call(operation);
    if (failure == operation) { errno = EACCES; return -1; }
    return 0;
}

static int mock_setgroups(int size, const gid_t *groups)
{
    assert(size == 0 && groups == NULL);
    return setter('s');
}

static int mock_setgid(gid_t gid)
{
    assert(gid == 20u);
    return setter('d');
}

static int mock_setuid(uid_t uid)
{
    if (uid == 0u)
    {
        call('r');
        assert(errno == 0);
        errno = regain_errno;
        return regain_result;
    }
    assert(uid == 501u);
    const int result = setter('u');
    if (result == 0) { effective_uid = failure == 'e' ? 0u : 501u; }
    return result;
}

static int mock_getgroups(int size, gid_t *groups)
{
    /* Successful group queries need not clear errno. */
    if (size == 0)
    {
        call('h');
        assert(groups == NULL);
        errno = group_count < 0 ? EINVAL : EIO;
        return group_count;
    }
    call('i');
    assert(size == 1 && groups != NULL);
    assert(group_count == 1);
    errno = fetch_count < 0 ? EACCES : EIO;
    if (fetch_count == 1) { *groups = fetched_gid; }
    return fetch_count;
}

#define getenv mock_getenv
#define geteuid mock_geteuid
#define getuid mock_getuid
#define getgid mock_getgid
#define getegid mock_getegid
#define setgroups mock_setgroups
#define setgid mock_setgid
#define setuid mock_setuid
#define getgroups mock_getgroups
#include "../tools/ethernet_raw_privilege.c"
#undef getenv
#undef geteuid
#undef getuid
#undef getgid
#undef getegid
#undef setgroups
#undef setgid
#undef setuid
#undef getgroups

static void reset(void)
{
    count = 0u;
    calls[0] = '\0';
    failure = '\0';
    uid_text = "501";
    gid_text = "20";
    real_uid = 501u;
    effective_uid = 0u;
    real_gid = effective_gid = 20u;
    group_count = fetch_count = 1;
    fetched_gid = 20u;
    regain_result = -1;
    regain_errno = EPERM;
}

static void check(bool expected, const char *order)
{
    assert(raw_drop_privileges() == expected);
    assert(strcmp(calls, order) == 0);
}

int main(void)
{
    const char *invalid[] = {
        NULL, "", "0", "000", "-1", "+1", " 501", "501 ", "1x",
        "4294967295", "4294967296", "184467440737095516160"
    };
    uint32_t value = 0u;
    for (size_t i = 0u; i < sizeof invalid / sizeof invalid[0]; ++i)
    {
        assert(!identity(invalid[i], &value));
        reset(); uid_text = invalid[i]; check(false, "ea");
        reset(); gid_text = invalid[i]; check(false, "eab");
    }
    assert(identity("000501", &value) && value == 501u);
    assert(identity("4294967294", &value) && value == UINT32_MAX - 1u);
    reset(); check(true, "eabsduUeGghir");
    /* Preserve the nonroot fast path, including no env/group/real-ID checks. */
    reset(); effective_uid = 501u; real_uid = 0u;
    uid_text = gid_text = NULL; group_count = 3; check(true, "e");
    reset(); failure = 's'; check(false, "eabs");
    reset(); failure = 'd'; check(false, "eabsd");
    reset(); failure = 'u'; check(false, "eabsdu");
    reset(); real_uid = 0u; check(false, "eabsduU");
    reset(); failure = 'e'; check(false, "eabsduUe");
    reset(); real_gid = 0u; check(false, "eabsduUeG");
    reset(); effective_gid = 0u; check(false, "eabsduUeGg");
    reset(); group_count = -1; check(false, "eabsduUeGgh");
    reset(); group_count = 0; check(false, "eabsduUeGgh");
    /* Even multiple copies of the target GID must fail at the count query. */
    reset(); group_count = 2; check(false, "eabsduUeGgh");
    reset(); group_count = 3; check(false, "eabsduUeGgh");
    reset(); fetch_count = -1; check(false, "eabsduUeGghi");
    reset(); fetch_count = 0; check(false, "eabsduUeGghi");
    reset(); fetch_count = 2; check(false, "eabsduUeGghi");
    reset(); fetched_gid = 0u; check(false, "eabsduUeGghi");
    reset(); fetched_gid = 502u; check(false, "eabsduUeGghi");
    reset(); regain_result = 0; check(false, "eabsduUeGghir");
    reset(); regain_result = 1; check(false, "eabsduUeGghir");
    reset(); regain_errno = EACCES; check(false, "eabsduUeGghir");
    reset(); regain_errno = 0; check(false, "eabsduUeGghir");
    reset(); check(true, "eabsduUeGghir");
    puts("raw privilege tests: PASS (mock syscalls only)");
    return 0;
}

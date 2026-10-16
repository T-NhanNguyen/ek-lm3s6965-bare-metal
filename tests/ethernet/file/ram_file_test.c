/* Offline native assertions; no board, transport, or filesystem dependencies. */
#include "lm3s6965/ram_file.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define TEST_CAPACITY 16u

typedef struct
{
    char prefix[80];
    ram_file_t file;
    uint8_t a[80];
    uint8_t b[80];
    char name[80];
} fixture_t;

static unsigned groups;

static void setup(fixture_t *f, size_t capacity, const char *name)
{
    memset(f, 0x5a, sizeof(*f));
    strcpy(f->name, name);
    assert(ram_file_init(&f->file, f->a, f->b, capacity, f->name));
    assert(ram_file_storage(&f->file) == &f->file.storage);
    assert(f->file.storage.context == &f->file);
    assert(f->file.storage.capacity == capacity);
}

static ftp_storage_view_t view(const ftp_storage_t *s)
{
    ftp_storage_view_t v;
    s->ops->inspect(s->context, &v);
    return v;
}

static void expect_view(const ftp_storage_t *s, const uint8_t *pointer,
                        const uint8_t *bytes, size_t length, bool exists)
{
    ftp_storage_view_t v = view(s);
    assert(v.exists == exists && v.length == length && v.data == pointer);
    if (length != 0u)
    {
        assert(memcmp(v.data, bytes, length) == 0);
    }
}

/* Save bytes, not struct assignments: include padding and every buffer byte. */
static void invalid(fixture_t *f, ram_file_t *file, uint8_t *a, uint8_t *b,
                    size_t capacity, const char *name)
{
    unsigned char saved[sizeof(*f)];
    memcpy(saved, f, sizeof(*f));
    assert(!ram_file_init(file, a, b, capacity, name));
    assert(memcmp(saved, f, sizeof(*f)) == 0);
}

static void test_names(void)
{
    fixture_t f;
    const ftp_storage_t *s;
    const char *bad[] = {"", ".", "..", "a/b", "a\\b", "a b", "a\tb",
                         "a\nb", "a%b", "a:b", "a+b", "\x7f", "\x80"};
    size_t i;
    char maximum[64];
    char oversized[65];
    char rooted[65];
    setup(&f, TEST_CAPACITY, "Abc-09_.txt");
    s = ram_file_storage(&f.file);
    assert(s->ops->matches_name(s->context, "Abc-09_.txt", 11u));
    assert(s->ops->matches_name(s->context, "/Abc-09_.txt", 12u));
    assert(!s->ops->matches_name(s->context, NULL, 11u));
    assert(!s->ops->matches_name(s->context, "", 0u));
    assert(!s->ops->matches_name(s->context, "/", 1u));
    assert(!s->ops->matches_name(s->context, "abc-09_.txt", 11u));
    assert(!s->ops->matches_name(s->context, "Abc-09_.txt/", 12u));
    assert(!s->ops->matches_name(s->context, "//Abc-09_.txt", 13u));
    assert(!s->ops->matches_name(s->context, " Abc-09_.txt", 12u));
    assert(!s->ops->matches_name(s->context, "Abc-09_\0txt", 11u));
    assert(!s->ops->matches_name(s->context, "Abc-09_.txt", SIZE_MAX));
    /* A non-NUL-terminated span is accepted. */
    {
        const char span[11] = {'A','b','c','-','0','9','_','.','t','x','t'};
        assert(s->ops->matches_name(s->context, span, sizeof(span)));
    }
    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY, bad[i]);
    }
    memset(maximum, 'x', sizeof(maximum));
    maximum[63] = '\0';
    memset(oversized, 'x', sizeof(oversized));
    oversized[64] = '\0';
    invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY, oversized);
    /* Exactly 64 readable non-NUL bytes: bounded scan must not read byte 65. */
    {
        char unterminated[64];
        memset(unterminated, 'x', sizeof(unterminated));
        invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY, unterminated);
    }
    setup(&f, 1u, maximum);
    s = ram_file_storage(&f.file);
    rooted[0] = '/';
    memcpy(rooted + 1, maximum, sizeof(maximum));
    assert(s->ops->matches_name(s->context, maximum, 63u));
    assert(s->ops->matches_name(s->context, rooted, 64u));
    assert(!s->ops->matches_name(s->context, rooted, 65u));
    /* Short allocation/scan and a valid one-character basename. */
    setup(&f, 1u, "z");
    assert(s->ops->matches_name(s->context, "z", 1u));
    ++groups;
}

static void test_invalid_ranges(void)
{
    fixture_t f;
    size_t maximum = SIZE_MAX;
    /* Invalid first initialization also preserves an uninitialized object. */
    memset(&f, 0x5a, sizeof(f));
    strcpy(f.name, "hello");
    invalid(&f, &f.file, f.a, f.b, 0u, f.name);
    invalid(&f, &f.file, f.a, f.a, TEST_CAPACITY, f.name);
    setup(&f, TEST_CAPACITY, "hello");
    assert(ram_file_storage(NULL) == NULL);
    invalid(&f, NULL, f.a, f.b, TEST_CAPACITY, f.name);
    invalid(&f, &f.file, NULL, f.b, TEST_CAPACITY, f.name);
    invalid(&f, &f.file, f.a, NULL, TEST_CAPACITY, f.name);
    invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY, NULL);
    invalid(&f, &f.file, f.a, f.b, 0u, f.name);
    /* Each of the six pairings: object/A, object/B, A/B, A/name,
     * B/name, object/name. Include interior, equality and NUL-only overlap. */
    invalid(&f, &f.file, (uint8_t *)&f.file, f.b, 1u, f.name);
    invalid(&f, &f.file, f.a, (uint8_t *)&f.file + sizeof(f.file) - 1u,
            1u, f.name);
    invalid(&f, &f.file, f.a, f.a, TEST_CAPACITY, f.name);
    invalid(&f, &f.file, f.a, f.a + TEST_CAPACITY - 1u,
            TEST_CAPACITY, f.name);
    invalid(&f, &f.file, (uint8_t *)f.name, f.b, 1u, f.name);
    invalid(&f, &f.file, (uint8_t *)f.name + strlen(f.name), f.b, 1u, f.name);
    invalid(&f, &f.file, f.a, (uint8_t *)f.name + strlen(f.name), 1u, f.name);
    strcpy((char *)&f.file, "inside");
    invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY, (char *)&f.file);
    /* Valid name characters before object, with only NUL inside the object. */
    {
        char *name = (char *)&f + offsetof(fixture_t, file) - 3u;
        memcpy(name, "abc", 4u);
        invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY, name);
    }
    setup(&f, TEST_CAPACITY, "hello");
    /* Synthetic addresses are rejected before any dereference. They do not
     * claim to be valid allocations; extent/alignment remain preconditions. */
    invalid(&f, (ram_file_t *)(uintptr_t)(UINTPTR_MAX - 1u), f.a, f.b,
            TEST_CAPACITY, f.name);
    invalid(&f, &f.file, (uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u), f.b,
            TEST_CAPACITY, f.name);
    invalid(&f, &f.file, f.a, (uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u),
            TEST_CAPACITY, f.name);
    invalid(&f, &f.file, f.a, f.b, TEST_CAPACITY,
            (const char *)(uintptr_t)UINTPTR_MAX);
    if ((uintmax_t)maximum > (uintmax_t)UINT_MAX)
    {
        maximum = (size_t)UINT_MAX;
    }
    if ((uintmax_t)maximum > (uintmax_t)UINT32_MAX)
    {
        maximum = (size_t)UINT32_MAX;
    }
    /* Maximum passes the integer bound but must still fail overlapping ranges.
     * No huge real allocation or invalid fake successful init is needed. */
    invalid(&f, &f.file, f.a, f.a, maximum, f.name);
    if (maximum < SIZE_MAX)
    {
        invalid(&f, &f.file, f.a, f.b, maximum + 1u, f.name);
    }
    invalid(&f, &f.file, f.a, f.b, SIZE_MAX, f.name);
    /* Adjacent ranges (including a basename terminator) are legal. */
    memset(&f, 0, sizeof(f));
    memcpy(f.a, "abc", 4u);
    assert(ram_file_init(&f.file, f.a + 4u, f.a + 8u, 4u, (char *)f.a));
    ++groups;
}

static void test_uploads(void)
{
    fixture_t f;
    const ftp_storage_t *s;
    const ftp_storage_ops_t *ops;
    const uint8_t binary[16] = {0u, 1u, 255u, 0u, 42u, 6u, 7u, 8u,
                                9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u};
    const uint8_t replacement[] = {55u, 0u, 99u};
    ftp_storage_view_t old;
    unsigned char saved[sizeof(f)];
    setup(&f, sizeof(binary), "hello");
    s = ram_file_storage(&f.file);
    ops = s->ops;
    expect_view(s, NULL, NULL, 0u, false);
    assert(ops->append(s->context, NULL, 0u) == FTP_STORAGE_INVALID_STATE);
    assert(ops->commit(s->context) == FTP_STORAGE_INVALID_STATE);
    ops->abort(s->context);
    ops->abort(s->context);
    assert(ops->begin(s->context) == FTP_STORAGE_OK);
    memcpy(saved, &f, sizeof(f));
    assert(ops->begin(s->context) == FTP_STORAGE_INVALID_STATE);
    assert(memcmp(saved, &f, sizeof(f)) == 0);
    assert(ops->append(s->context, NULL, 0u) == FTP_STORAGE_OK);
    assert(ops->append(s->context, NULL, 1u) == FTP_STORAGE_INVALID_ARGUMENT);
    expect_view(s, NULL, NULL, 0u, false);
    assert(ops->commit(s->context) == FTP_STORAGE_OK);
    expect_view(s, f.b, NULL, 0u, true);
    assert(ops->begin(s->context) == FTP_STORAGE_OK);
    assert(ops->append(s->context, binary, 5u) == FTP_STORAGE_OK);
    memcpy(saved, &f, sizeof(f));
    assert(ops->append(s->context, binary, 12u) == FTP_STORAGE_CAPACITY);
    assert(memcmp(saved, &f, sizeof(f)) == 0);
    expect_view(s, f.b, NULL, 0u, true);
    assert(ops->append(s->context, binary + 5u, 11u) == FTP_STORAGE_OK);
    memcpy(saved, &f, sizeof(f));
    assert(ops->append(s->context, replacement, 1u) == FTP_STORAGE_CAPACITY);
    assert(memcmp(saved, &f, sizeof(f)) == 0);
    assert(ops->append(s->context, NULL, 0u) == FTP_STORAGE_OK);
    assert(ops->commit(s->context) == FTP_STORAGE_OK);
    expect_view(s, f.a, binary, sizeof(binary), true);
    old = view(s);
    assert(ops->begin(s->context) == FTP_STORAGE_OK);
    assert(ops->append(s->context, replacement, sizeof(replacement)) == FTP_STORAGE_OK);
    expect_view(s, old.data, binary, sizeof(binary), true);
    ops->abort(s->context);
    ops->abort(s->context);
    expect_view(s, old.data, binary, sizeof(binary), true);
    assert(ops->commit(s->context) == FTP_STORAGE_INVALID_STATE);
    expect_view(s, old.data, binary, sizeof(binary), true);
    assert(ops->begin(s->context) == FTP_STORAGE_OK);
    assert(ops->append(s->context, replacement, sizeof(replacement)) == FTP_STORAGE_OK);
    assert(ops->commit(s->context) == FTP_STORAGE_OK);
    expect_view(s, f.b, replacement, sizeof(replacement), true);
    assert(ops->begin(s->context) == FTP_STORAGE_OK);
    assert(ops->commit(s->context) == FTP_STORAGE_OK);
    expect_view(s, f.a, NULL, 0u, true);
    assert(ops->begin(s->context) == FTP_STORAGE_OK);
    assert(ops->append(s->context, binary, 2u) == FTP_STORAGE_OK);
    ops->clear(s->context);
    expect_view(s, NULL, NULL, 0u, false);
    assert(ops->commit(s->context) == FTP_STORAGE_INVALID_STATE);
    assert(ops->matches_name(s->context, "/hello", 6u));
    assert(s->capacity == sizeof(binary));
    ops->clear(s->context);
    ++groups;
}

static void test_source_aliases(void)
{
    fixture_t f;
    const ftp_storage_t *s;
    const uint8_t byte = 12u;
    const uint8_t *bad[7];
    size_t lengths[7] = {1u, 1u, 1u, 1u, 1u, 2u, SIZE_MAX};
    size_t i;
    unsigned char saved[sizeof(f)];
    setup(&f, TEST_CAPACITY, "hello");
    s = ram_file_storage(&f.file);
    assert(s->ops->begin(s->context) == FTP_STORAGE_OK);
    assert(s->ops->append(s->context, &byte, 1u) == FTP_STORAGE_OK);
    assert(s->ops->commit(s->context) == FTP_STORAGE_OK);
    bad[0] = view(s).data; /* Old committed read view: explicitly forbidden. */
    bad[1] = f.a;          /* Staging destination. */
    bad[2] = f.a + TEST_CAPACITY - 1u; /* Unused buffer tail also forbidden. */
    bad[3] = (const uint8_t *)&f.file;
    bad[4] = (const uint8_t *)&f.file.storage;
    bad[5] = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    bad[6] = &byte;
    assert(s->ops->begin(s->context) == FTP_STORAGE_OK);
    assert(s->ops->append(s->context, &byte, 1u) == FTP_STORAGE_OK);
    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        memcpy(saved, &f, sizeof(f));
        assert(s->ops->append(s->context, bad[i], lengths[i]) ==
               FTP_STORAGE_INVALID_ARGUMENT);
        assert(memcmp(saved, &f, sizeof(f)) == 0);
        expect_view(s, f.b, &byte, 1u, true);
    }
    /* Source crosses into buffer from immediately before it. */
    memcpy(saved, &f, sizeof(f));
    assert(s->ops->append(s->context,
                          (const uint8_t *)&f + offsetof(fixture_t, b) - 1u, 2u) == FTP_STORAGE_INVALID_ARGUMENT);
    assert(memcmp(saved, &f, sizeof(f)) == 0);
    /* Zero length never dereferences or copies even an aliased pointer. */
    assert(s->ops->append(s->context, view(s).data, 0u) == FTP_STORAGE_OK);
    /* Borrowed immutable name is not mutable state, so is a legal source. */
    assert(s->ops->append(s->context, (const uint8_t *)f.name, 5u) == FTP_STORAGE_OK);
    assert(s->ops->commit(s->context) == FTP_STORAGE_OK);
    {
        const uint8_t expected[] = {12u, 'h', 'e', 'l', 'l', 'o'};
        expect_view(s, f.a, expected, sizeof(expected), true);
    }
    ++groups;
}

static void test_independence_and_reinit(void)
{
    fixture_t a;
    fixture_t b;
    const ftp_storage_t *sa;
    const ftp_storage_t *sb;
    const uint8_t bytes[] = {1u, 2u};
    uint8_t saved_a[sizeof(a.a)];
    uint8_t saved_b[sizeof(a.b)];
    setup(&a, 1u, "one");
    setup(&b, 7u, "two.bin");
    sa = ram_file_storage(&a.file);
    sb = ram_file_storage(&b.file);
    assert(sa != sb && sa->context != sb->context && sa->ops == sb->ops);
    assert(!sa->ops->matches_name(sa->context, "two.bin", 7u));
    assert(!sb->ops->matches_name(sb->context, "one", 3u));
    assert(sa->ops->begin(sa->context) == FTP_STORAGE_OK);
    assert(sb->ops->begin(sb->context) == FTP_STORAGE_OK);
    assert(sa->ops->append(sa->context, bytes, 2u) == FTP_STORAGE_CAPACITY);
    assert(sb->ops->append(sb->context, bytes, 2u) == FTP_STORAGE_OK);
    assert(sa->ops->append(sa->context, bytes, 1u) == FTP_STORAGE_OK);
    assert(sa->ops->commit(sa->context) == FTP_STORAGE_OK);
    assert(sb->ops->commit(sb->context) == FTP_STORAGE_OK);
    invalid(&a, &a.file, a.a, a.b, 0u, a.name);
    expect_view(sa, a.b, bytes, 1u, true);
    sb->ops->clear(sb->context);
    expect_view(sa, a.b, bytes, 1u, true);
    /* Quiescent reinit: no bound transport or outstanding view. No erasure. */
    memcpy(saved_a, a.a, sizeof(a.a));
    memcpy(saved_b, a.b, sizeof(a.b));
    assert(ram_file_init(&a.file, a.a, a.b, 3u, "new"));
    assert(memcmp(saved_a, a.a, sizeof(a.a)) == 0);
    assert(memcmp(saved_b, a.b, sizeof(a.b)) == 0);
    assert(ram_file_storage(&a.file) == sa && sa->capacity == 3u);
    expect_view(sa, NULL, NULL, 0u, false);
    assert(sa->ops->matches_name(sa->context, "/new", 4u));
    assert(!sa->ops->matches_name(sa->context, "one", 3u));
    assert(sa->ops->begin(sa->context) == FTP_STORAGE_OK);
    assert(sa->ops->append(sa->context, bytes, 2u) == FTP_STORAGE_OK);
    assert(sa->ops->commit(sa->context) == FTP_STORAGE_OK);
    expect_view(sa, a.b, bytes, 2u, true);
    expect_view(sb, NULL, NULL, 0u, false);
    ++groups;
}

int main(void)
{
    test_names();
    test_invalid_ranges();
    test_uploads();
    test_source_aliases();
    test_independence_and_reinit();
    printf("ram_file_test: %u groups passed\n", groups);
    return 0;
}

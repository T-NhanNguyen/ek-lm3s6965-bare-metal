/* Native macOS raw-frame experiment. No IP, flag changes or promiscuous mode.
 * An opened BPF descriptor remains a raw-frame capability after privilege drop.
 * This is a diagnostic, not authenticated evidence or a network stack test. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ethernet_raw_capture.h"
#include "ethernet_raw_privilege.h"
#include "../../../../examples/ethernet-raw/raw_protocol.h"

#define REQUEST_COUNT 3u
#define TIMEOUT_MS 3000

static bool control(int fd, unsigned long command, void *argument)
{
    int result;
    do { result = ioctl(fd, command, argument); }
    while (result < 0 && errno == EINTR);
    if (result < 0) { perror("BPF configuration"); }
    return result == 0;
}

static bool interface_mac(const char *name, uint8_t mac[6])
{
    struct ifaddrs *addresses;
    if (getifaddrs(&addresses) != 0) { perror("getifaddrs"); return false; }
    bool found = false;
    for (const struct ifaddrs *a = addresses; a != NULL; a = a->ifa_next)
    {
        if (a->ifa_addr == NULL || strcmp(a->ifa_name, name) != 0 ||
            a->ifa_addr->sa_family != AF_LINK) { continue; }
        const struct sockaddr_dl *dl = (const void *)a->ifa_addr;
        const size_t start = offsetof(struct sockaddr_dl, sdl_data);
        if (dl->sdl_len < start || dl->sdl_type != IFT_ETHER ||
            dl->sdl_alen != 6u ||
            (size_t)dl->sdl_nlen + 6u > dl->sdl_len - start) { continue; }
        memcpy(mac, LLADDR(dl), 6u);
        uint8_t request[RAW_FRAME_BYTES], nonce[RAW_NONCE_BYTES] = {0};
        found = raw_build_request(request, sizeof request, mac, 1u, nonce);
        break;
    }
    freeifaddrs(addresses);
    if (!found)
    {
        fprintf(stderr, "%s: need Ethernet and a nonzero unicast MAC distinct "
                "from the experiment board\n", name);
    }
    return found;
}

static int open_bpf(void)
{
    /* Busy minors are skipped; cap enumeration to avoid unbounded probing. */
    for (unsigned i = 0u; i < 4096u; ++i)
    {
        char path[32];
        (void)snprintf(path, sizeof path, "/dev/bpf%u", i);
        int fd;
        do { fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK); }
        while (fd < 0 && errno == EINTR);
        if (fd >= 0)
        {
            const int flags = fcntl(fd, F_GETFD);
            if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
            {
                perror("BPF close-on-exec");
                (void)close(fd);
                return -1;
            }
            return fd;
        }
        if (errno == EBUSY) { continue; }
        if (errno == EACCES || errno == EPERM)
        {
            fprintf(stderr, "%s: need read/write permission on one available "
                    "BPF device. No permission changes or sudo are performed. "
                    "Build unprivileged, then explicitly run the helper with "
                    "approved elevation if needed.\n", path);
        }
        else { perror(path); }
        return -1;
    }
    fprintf(stderr, "No available BPF device within 4096 minors\n");
    return -1;
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        perror("CLOCK_MONOTONIC");
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_ready(int fd, short events, int64_t deadline)
{
    for (;;)
    {
        const int64_t now = milliseconds();
        if (now < 0) { return -1; }
        if (now >= deadline) { return 0; }
        struct pollfd descriptor = {.fd = fd, .events = events};
        const int result = poll(&descriptor, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) { continue; }
        if (result < 0) { perror("poll BPF"); return -1; }
        if (result == 0) { continue; }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            fprintf(stderr, "BPF poll descriptor error\n");
            return -1;
        }
        if ((descriptor.revents & events) != 0) { return 1; }
    }
}

static bool exchange(int fd, uint8_t *buffer, size_t size,
                     const uint8_t host[6], const raw_message_t *message)
{
    uint8_t request[RAW_FRAME_BYTES], reply[RAW_FRAME_BYTES];
    if (!raw_build_request(request, sizeof request, host, message->sequence,
                           message->nonce) ||
        !raw_build_reply(request, sizeof request, reply, sizeof reply))
    {
        return false;
    }
    struct bpf_insn instructions[RAW_FILTER_INSNS];
    raw_reply_filter(instructions, reply);
    struct bpf_program filter = {RAW_FILTER_INSNS, instructions};
    /* BIOCSETF flushes capture and arms the exact reply before injection. */
    if (!control(fd, BIOCSETF, &filter)) { return false; }
    const int64_t start = milliseconds();
    if (start < 0) { return false; }
    const int64_t deadline = start + TIMEOUT_MS;
    for (;;)
    {
        if (wait_ready(fd, POLLOUT, deadline) != 1) { return false; }
        const ssize_t sent = write(fd, request, sizeof request);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN)) { continue; }
        if (sent != (ssize_t)sizeof request)
        {
            /* Never retry a short write as a second frame. */
            if (sent < 0) { perror("write BPF"); }
            else { fprintf(stderr, "Short raw-frame write\n"); }
            return false;
        }
        break;
    }
    for (;;)
    {
        if (wait_ready(fd, POLLIN, deadline) != 1) { return false; }
        const ssize_t received = read(fd, buffer, size);
        if (received < 0 && (errno == EINTR || errno == EAGAIN)) { continue; }
        if (received < 0) { perror("read BPF"); return false; }
        if (received == 0) { continue; }
        bool matched;
        if (!raw_capture_match(buffer, (size_t)received, host, message,
                               &matched))
        {
            fprintf(stderr, "Malformed/truncated BPF record buffer\n");
            return false;
        }
        if (matched) { return true; }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0)
    {
        puts("Usage: ethernet_raw_test [interface] (default en7)\n"
             "Sends 3 sequential exact 60-byte requests; 3s per request.\n"
             "Build/self-test offline: scripts/ethernet/raw/ethernet-raw-test.sh --help\n"
             "No sudo, permission installer or network-setting changes.");
        return 0;
    }
    const char *name = argc == 2 ? argv[1] : "en7";
    if (argc > 2 || *name == '\0' || *name == '-' ||
        strlen(name) >= IFNAMSIZ)
    {
        fprintf(stderr, "Usage: ethernet_raw_test [interface]\n");
        return 2;
    }
    uint8_t host[6];
    if (!interface_mac(name, host)) { return 1; }
    const int fd = open_bpf();
    if (fd < 0) { return 1; }
    int result = 1;
    uint8_t *buffer = NULL;
    struct ifreq interface = {0};
    memcpy(interface.ifr_name, name, strlen(name) + 1u);
    u_int dlt = 0u, size = 0u, one = 1u;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    struct bpf_version version;
    /* Reject everything before attachment; never briefly capture unrelated
     * traffic with the default accept-all filter. */
    struct bpf_insn reject = BPF_STMT(BPF_RET | BPF_K, 0);
    struct bpf_program initial = {1u, &reject};
    if (!control(fd, BIOCVERSION, &version)) { goto cleanup; }
    if (version.bv_major != BPF_MAJOR_VERSION ||
        version.bv_minor < BPF_MINOR_VERSION)
    {
        fprintf(stderr, "Incompatible BPF filter ABI\n");
        goto cleanup;
    }
    if (!control(fd, BIOCSETF, &initial) ||
        !control(fd, BIOCSETIF, &interface) ||
        !control(fd, BIOCGDLT, &dlt)) { goto cleanup; }
    if (dlt != DLT_EN10MB)
    {
        fprintf(stderr, "%s: not DLT_EN10MB Ethernet\n", name);
        goto cleanup;
    }
    if (!control(fd, BIOCGBLEN, &size) ||
        !control(fd, BIOCIMMEDIATE, &one) ||
        !control(fd, BIOCSHDRCMPLT, &one) ||
        !control(fd, BIOCSRTIMEOUT, &timeout)) { goto cleanup; }
#ifdef BIOCSSEESENT
    u_int zero = 0u;
    if (!control(fd, BIOCSSEESENT, &zero)) { goto cleanup; }
#endif
    if (size < sizeof(struct bpf_hdr) + RAW_FRAME_BYTES ||
        size > BPF_MAXBUFSIZE)
    {
        fprintf(stderr, "Unexpected BPF buffer size\n");
        goto cleanup;
    }
    buffer = malloc(size);
    if (buffer == NULL) { perror("malloc"); goto cleanup; }
    if (!raw_drop_privileges()) { goto cleanup; }
    raw_message_t message = {.sequence = 0u};
    arc4random_buf(message.nonce, sizeof message.nonce);
    printf("raw interface=%s requests=%u bytes=60 (DA through payload, no FCS) "
           "timeout_ms=%d nonce=", name, REQUEST_COUNT, TIMEOUT_MS);
    for (size_t n = 0u; n < sizeof message.nonce; ++n)
    {
        printf("%02X", (unsigned)message.nonce[n]);
    }
    putchar('\n');
    for (unsigned i = 0u; i < REQUEST_COUNT; ++i)
    {
        message.sequence = i + 1u;
        if (!exchange(fd, buffer, size, host, &message))
        {
            fprintf(stderr, "RAW FRAME FAIL matched=%u/%u seq=%" PRIu32
                    " (timeout or I/O/validation failure)\n",
                    i, REQUEST_COUNT, message.sequence);
            goto cleanup;
        }
        printf("raw reply matched=%u/%u seq=%" PRIu32 " bytes=60\n",
               i + 1u, REQUEST_COUNT, message.sequence);
    }
    puts("RAW FRAME PASS: 3/3 bidirectional exact 60-byte exchanges.\n"
         "Not proof of other lengths, padding, full MTU, IP or FTP.");
    result = 0;
cleanup:
    free(buffer);
    (void)close(fd);
    return result;
}

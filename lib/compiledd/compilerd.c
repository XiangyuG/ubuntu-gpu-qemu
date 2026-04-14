// SPDX-License-Identifier: MIT
/*
 * compilerd - RVT2 IR compilation service daemon.
 *
 * Request protocol over a Unix domain socket:
 *   COMPILE <byte-count>\n
 *   <byte-count bytes of text IR>
 *
 * Response:
 *   OK <byte-count>\n
 *   <byte-count bytes of binary descriptors>
 *
 * or:
 *   ERR <byte-count>\n
 *   <byte-count bytes of diagnostic text>
 */

#include "rvt2_compile.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define RVT2_COMPILERD_DEFAULT_SOCKET "/tmp/rvt2_compilerd.sock"
#define RVT2_COMPILERD_MAX_IR (1024 * 1024)
#define RVT2_COMPILERD_BACKLOG 16

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: compilerd [--socket PATH] [--once] [--daemonize] "
            "[--pid-file PATH]\n"
            "Protocol: send 'COMPILE <len>\\n' followed by <len> bytes of IR.\n");
}

static ssize_t retry_read(int fd, void *buf, size_t len)
{
    ssize_t ret;

    do {
        ret = read(fd, buf, len);
    } while (ret < 0 && errno == EINTR && !stop_requested);

    return ret;
}

static ssize_t retry_write(int fd, const void *buf, size_t len)
{
    ssize_t ret;

    do {
        ret = write(fd, buf, len);
    } while (ret < 0 && errno == EINTR && !stop_requested);

    return ret;
}

static int read_exact(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t ret = retry_read(fd, p, len);

        if (ret == 0)
            return -ECONNRESET;
        if (ret < 0)
            return -errno;

        p += ret;
        len -= (size_t)ret;
    }

    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t ret = retry_write(fd, p, len);

        if (ret < 0)
            return -errno;

        p += ret;
        len -= (size_t)ret;
    }

    return 0;
}

static int read_header(int fd, char *header, size_t header_len)
{
    size_t pos = 0;

    while (pos + 1 < header_len) {
        char ch;
        ssize_t ret = retry_read(fd, &ch, 1);

        if (ret == 0)
            return pos == 0 ? 0 : -ECONNRESET;
        if (ret < 0)
            return -errno;

        header[pos++] = ch;
        if (ch == '\n') {
            header[pos] = '\0';
            return 1;
        }
    }

    header[header_len - 1] = '\0';
    return -E2BIG;
}

static int send_blob(int fd, const char *status,
                     const void *payload, size_t payload_len)
{
    char header[64];
    int header_len;

    header_len = snprintf(header, sizeof(header), "%s %zu\n",
                          status, payload_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header))
        return -EOVERFLOW;

    if (write_all(fd, header, (size_t)header_len) < 0)
        return -EIO;
    if (payload_len && write_all(fd, payload, payload_len) < 0)
        return -EIO;

    return 0;
}

static int send_error(int fd, const char *message)
{
    if (!message)
        message = "error: request failed\n";
    return send_blob(fd, "ERR", message, strlen(message));
}

static int parse_compile_header(const char *header, size_t *len)
{
    const char *p;
    char *end;
    unsigned long long value;

    if (strncmp(header, "COMPILE ", 8) != 0)
        return -EINVAL;

    p = header + 8;
    errno = 0;
    value = strtoull(p, &end, 10);
    if (errno || end == p || (*end != '\n' && *end != '\0'))
        return -EINVAL;
    if (value > RVT2_COMPILERD_MAX_IR)
        return -EFBIG;

    *len = (size_t)value;
    return 0;
}

static int handle_client(int client_fd)
{
    char header[128];
    char *ir = NULL;
    unsigned char *output = NULL;
    char *log = NULL;
    size_t ir_len = 0;
    size_t output_len = 0;
    int count = 0, errors = 0;
    int ret;

    ret = read_header(client_fd, header, sizeof(header));
    if (ret <= 0)
        return ret;

    if (strcmp(header, "PING\n") == 0)
        return send_blob(client_fd, "OK", "PONG", 4);

    ret = parse_compile_header(header, &ir_len);
    if (ret == -EFBIG)
        return send_error(client_fd, "error: IR payload is too large\n");
    if (ret)
        return send_error(client_fd, "error: expected COMPILE <len> header\n");

    ir = malloc(ir_len ? ir_len : 1);
    if (!ir)
        return send_error(client_fd, "error: out of memory\n");

    ret = read_exact(client_fd, ir, ir_len);
    if (ret) {
        free(ir);
        return ret;
    }

    ret = rvt2_compile_ir_buffer(ir, ir_len, &output, &output_len,
                                 &log, &count, &errors);
    free(ir);

    if (ret) {
        ret = send_error(client_fd, log);
    } else {
        ret = send_blob(client_fd, "OK", output, output_len);
    }

    rvt2_compile_free(output);
    rvt2_compile_free(log);
    (void)count;
    (void)errors;
    return ret;
}

static int daemonize_process(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0)
        return -errno;
    if (pid > 0)
        _exit(0);

    if (setsid() < 0)
        return -errno;

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    return 0;
}

static int write_pid_file(const char *path)
{
    FILE *f;

    if (!path)
        return 0;

    f = fopen(path, "w");
    if (!f)
        return -errno;
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    return 0;
}

static int make_server_socket(const char *socket_path)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "compilerd: socket path too long: %s\n", socket_path);
        return -ENAMETOOLONG;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, socket_path);

    unlink(socket_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        return -saved;
    }

    if (listen(fd, RVT2_COMPILERD_BACKLOG) < 0) {
        int saved = errno;
        close(fd);
        unlink(socket_path);
        return -saved;
    }

    return fd;
}

int main(int argc, char **argv)
{
    const char *socket_path = RVT2_COMPILERD_DEFAULT_SOCKET;
    const char *pid_file = NULL;
    bool once = false;
    bool daemonize = false;
    int server_fd;
    int ret;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--pid-file") == 0 && i + 1 < argc) {
            pid_file = argv[++i];
        } else if (strcmp(argv[i], "--once") == 0) {
            once = true;
        } else if (strcmp(argv[i], "--daemonize") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    server_fd = make_server_socket(socket_path);
    if (server_fd < 0) {
        fprintf(stderr, "compilerd: failed to listen on %s: %s\n",
                socket_path, strerror(-server_fd));
        return 1;
    }

    if (daemonize) {
        ret = daemonize_process();
        if (ret) {
            fprintf(stderr, "compilerd: failed to daemonize: %s\n",
                    strerror(-ret));
            close(server_fd);
            unlink(socket_path);
            return 1;
        }
    }

    ret = write_pid_file(pid_file);
    if (ret) {
        fprintf(stderr, "compilerd: failed to write pid file: %s\n",
                strerror(-ret));
        close(server_fd);
        unlink(socket_path);
        return 1;
    }

    while (!stop_requested) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        (void)handle_client(client_fd);
        close(client_fd);

        if (once)
            break;
    }

    close(server_fd);
    unlink(socket_path);
    return 0;
}

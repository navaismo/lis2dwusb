#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../include/lis2dw_protocol.h"

enum State { HELLO, READY, STREAMING, STOPPING, COMPLETE, FAILED };
struct Capture {
    enum State state;
    int fd;
    unsigned rate;
    char token[9];
    const char *error;
    FILE *csv;
    uint64_t samples, first, last, max_window;
    double timeout, deadline, started;
    bool start_sent;
};
static volatile sig_atomic_t interrupted;

static void on_signal(int signal_number) { (void)signal_number; interrupted = 1; }
static double now(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) { perror("clock_gettime"); exit(2); }
    return value.tv_sec + value.tv_nsec / 1e9;
}
static void fail(struct Capture *capture, const char *reason) {
    if (capture->state != FAILED) capture->error = reason;
    capture->state = FAILED;
}
static bool number(const char *text, uint64_t *value) {
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    unsigned long long result = strtoull(text, &end, 10);
    if (errno || *end) return false;
    *value = result;
    return true;
}
static bool axis_value(const char *text, long *value) {
    const char *digits = text && text[0] == '-' ? text + 1 : text;
    uint64_t magnitude;
    if (!number(digits, &magnitude) || magnitude > 32768) return false;
    *value = text[0] == '-' ? -(long)magnitude : (long)magnitude;
    return *value >= -32768 && *value <= 32767;
}
static bool send_command(struct Capture *capture, const char *payload) {
    char frame[LIS_LINE_SIZE];
    int length = lis_frame(frame, sizeof(frame), payload);
    if (length < 0) return false;
    size_t offset = 0;
    double deadline = now() + capture->timeout;
    while (offset < (size_t)length && now() < deadline) {
        ssize_t sent = write(capture->fd, frame + offset, (size_t)length - offset);
        if (sent > 0) { offset += (size_t)sent; continue; }
        if (sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        struct pollfd output = {capture->fd, POLLOUT, 0};
        if (poll(&output, 1, 20) < 0 && errno != EINTR) return false;
        if (output.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    }
    return offset == (size_t)length;
}
static bool stop(struct Capture *capture) {
    char command[40];
    snprintf(command, sizeof(command), "STOP,%s", capture->token);
    if (!send_command(capture, command)) { fail(capture, "Could not send STOP"); return false; }
    capture->state = STOPPING;
    capture->deadline = now() + capture->timeout;
    return true;
}
static void line_received(struct Capture *capture, char *line) {
    if (!lis_unframe(line)) {
        if (capture->state == HELLO) {
            if (!strncmp(line, "time,", 5)) fail(capture, "Legacy firmware detected; upgrade both firmware and wrapper");
            return;  // Drain stale boot/stream bytes until the nonce-matched greeting.
        }
        fail(capture, "Invalid frame checksum or format"); return;
    }
    char *fields[10], *rest = line;
    unsigned count = 0;
    while (rest && count < 10) fields[count++] = strsep(&rest, ",");
    if (rest) { fail(capture, "Too many frame fields"); return; }
    if (count == 3 && !strcmp(fields[0], "ERROR") && !strcmp(fields[1], capture->token)) {
        fprintf(stderr, "Device error: %s\n", fields[2]);
        fail(capture, "Firmware rejected or invalidated this capture"); return;
    }
    if (capture->state == HELLO) {
        if (count != 3 || strcmp(fields[0], "LIS2DWUSB") || strcmp(fields[2], capture->token)) return;
        if (strcmp(fields[1], "2")) { fail(capture, "Unsupported device protocol version"); return; }
        char command[48];
        snprintf(command, sizeof(command), "START,%u,%s", capture->rate, capture->token);
        if (!send_command(capture, command)) { fail(capture, "Could not send START"); return; }
        capture->start_sent = true;
        capture->state = READY;
        capture->deadline = now() + capture->timeout;
        return;
    }
    if (capture->state == READY) {
        uint64_t actual, scale;
        if (count != 6 || strcmp(fields[0], "READY") || strcmp(fields[1], "2")
                || strcmp(fields[2], capture->token) || !number(fields[3], &actual)
                || actual != capture->rate || !number(fields[4], &scale)
                || scale != LIS_COUNTS_PER_G || strcmp(fields[5], "POLL")) {
            fail(capture, "Invalid READY: session, rate, scale or acquisition clock mismatch"); return;
        }
        capture->state = STREAMING;
        capture->started = now();
        capture->deadline = now() + capture->timeout;
        fprintf(stderr, "Protocol 2: %u Hz, device FIFO-edge timing, acceleration in g\nPress Q to stop\n", capture->rate);
        return;
    }
    if (count == 3 && !strcmp(fields[0], "DONE")) {
        uint64_t total;
        if (capture->state != STOPPING || strcmp(fields[1], capture->token)
                || !number(fields[2], &total) || total != capture->samples || !total) {
            fail(capture, "Invalid DONE: incomplete capture or mismatched sample count"); return;
        }
        capture->state = COMPLETE;
        return;
    }
    uint64_t sequence, timestamp, window;
    long axes[3];
    if (count != 7 || strcmp(fields[0], "S") || !number(fields[1], &sequence)
            || !number(fields[2], &timestamp) || !axis_value(fields[3], &axes[0])
            || !axis_value(fields[4], &axes[1]) || !axis_value(fields[5], &axes[2])
            || !number(fields[6], &window)) {
        fprintf(stderr, "Rejected sample payload (%u fields): ", count);
        for (unsigned i = 0; i < count; ++i) fprintf(stderr, "%s%s", i ? "," : "", fields[i]);
        fputc('\n', stderr);
        fail(capture, "Malformed sample frame"); return;
    }
    if (sequence != capture->samples || sequence >= UINT32_MAX) {
        fail(capture, "Sample sequence gap or duplicate"); return;
    }
    if (window > 1000000UL / capture->rate / 4 || !timestamp
            || (capture->samples && (timestamp <= capture->last
                || (timestamp - capture->last) * capture->rate < 500000
                || timestamp - capture->last > 1500000UL / capture->rate))) {
        fail(capture, "Invalid acquisition timing or uncertain sample edge"); return;
    }
    for (unsigned i = 0; i < 3; ++i)
        if (axes[i] >= 32760 || axes[i] <= -32760) { fail(capture, "Sensor full-scale clipping"); return; }
    if (fprintf(capture->csv, "%" PRIu64 ".%06" PRIu64 ",%.10f,%.10f,%.10f\n",
            timestamp / 1000000, timestamp % 1000000,
            axes[0] / (double)LIS_COUNTS_PER_G, axes[1] / (double)LIS_COUNTS_PER_G,
            axes[2] / (double)LIS_COUNTS_PER_G) < 0) {
        fail(capture, "CSV write failed"); return;
    }
    if (!capture->samples) capture->first = timestamp;
    capture->last = timestamp;
    if (window > capture->max_window) capture->max_window = window;
    ++capture->samples;
    if (!(capture->samples % 64) && fflush(capture->csv)) { fail(capture, "CSV flush failed"); return; }
    if (capture->state == STREAMING) capture->deadline = now() + capture->timeout;
}
static bool metadata(const char *path, const struct Capture *capture) {
    char *temporary;
    if (asprintf(&temporary, "%s.tmp.XXXXXX", path) < 0) return false;
    int fd = mkstemp(temporary);
    FILE *file = fd >= 0 ? fdopen(fd, "w") : NULL;
    bool ok = false;
    if (file) {
        double effective = capture->samples > 1 ?
            (capture->samples - 1) * 1e6 / (capture->last - capture->first) : 0;
        ok = fprintf(file, "{\n  \"protocol\": 2, \"valid\": %s, \"session\": \"%s\",\n"
            "  \"requested_rate\": %u, \"effective_rate\": %.6f, \"samples\": %" PRIu64 ",\n"
            "  \"clock\": \"rp2040_fifo_poll\", \"max_window_us\": %" PRIu64 ",\n"
            "  \"units\": \"g\", \"counts_per_g\": 16384\n}\n",
            capture->state == COMPLETE ? "true" : "false", capture->token,
            capture->rate, effective, capture->samples, capture->max_window) > 0;
        if (fflush(file) || fsync(fd)) ok = false;
        if (fclose(file)) ok = false;
        if (ok && rename(temporary, path)) ok = false;
    } else if (fd >= 0) close(fd);
    if (!ok) unlink(temporary);
    free(temporary);
    return ok;
}
static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [-p PORT] [-f 200|400|800|1600] [-s FILE] [-t SECONDS] [-d SECONDS]\n"
        "Defaults: /dev/ttyACM0, 1600 Hz, 3-second timeout. Q/Ctrl+C stops capture.\n"
        "-d sets a host duration; -s writes CSV plus FILE.meta.json. Protocol 2 firmware is required.\n", program);
}
static bool seconds(const char *text, double *value, bool allow_zero) {
    errno = 0;
    char *end;
    *value = strtod(text, &end);
    return !errno && text[0] && !*end && *value >= (allow_zero ? 0 : 0.1) && *value <= 86400;
}
int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, "C");
    struct Capture capture = {.state = HELLO, .fd = -1, .rate = 1600, .timeout = 3};
    const char *port = "/dev/ttyACM0", *path = NULL;
    double duration = 0;
    int option;
    while ((option = getopt(argc, argv, "hp:f:s:t:d:V")) != -1) {
        uint64_t rate;
        switch (option) {
        case 'h': usage(argv[0]); return 0;
        case 'V': puts("lis2dwusb 0.3.0 (protocol 2)"); return 0;
        case 'p': port = optarg; break;
        case 's': path = optarg; break;
        case 'f':
            if (!number(optarg, &rate) || rate > 1600 || !lis_rate_valid((unsigned)rate)) {
                fprintf(stderr, "Frequency must be one of 200, 400, 800 or 1600 Hz\n"); return 1;
            }
            capture.rate = (unsigned)rate; break;
        case 't': if (!seconds(optarg, &capture.timeout, false)) { usage(argv[0]); return 1; } break;
        case 'd': if (!seconds(optarg, &duration, true)) { usage(argv[0]); return 1; } break;
        default: usage(argv[0]); return 1;
        }
    }
    if (optind != argc) { usage(argv[0]); return 1; }
    uint32_t nonce;
    if (getrandom(&nonce, sizeof(nonce), 0) != sizeof(nonce)) { perror("getrandom"); return 2; }
    snprintf(capture.token, sizeof(capture.token), "%08" PRIx32, nonce);
    char *meta_path = NULL;
    if (path && asprintf(&meta_path, "%s.meta.json", path) < 0) return 2;
    if (meta_path && access(meta_path, F_OK) == 0) { fprintf(stderr, "Metadata file already exists\n"); free(meta_path); return 2; }
    capture.csv = stdout;
    if (path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        capture.csv = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (!capture.csv) { if (fd >= 0) close(fd); perror("create CSV"); free(meta_path); return 2; }
    }
    if (fprintf(capture.csv, "time,x,y,z\n") < 0 || fflush(capture.csv)) fail(&capture, "Cannot write CSV header");
    struct termios original, raw;
    bool terminal_changed = false;
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    capture.fd = capture.state == FAILED ? -1 : open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (capture.fd < 0) fail(&capture, "Cannot open sensor serial port");
    if (capture.state != FAILED) {
        if (tcgetattr(capture.fd, &raw)) fail(&capture, "Cannot read sensor port configuration");
        else {
            cfmakeraw(&raw);
            raw.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
            raw.c_cflag |= CS8 | CLOCAL | CREAD;
            raw.c_cc[VMIN] = raw.c_cc[VTIME] = 0;
            if (cfsetispeed(&raw, B2000000) || cfsetospeed(&raw, B2000000)
                    || tcsetattr(capture.fd, TCSANOW, &raw) || ioctl(capture.fd, TIOCEXCL)
                    || tcflush(capture.fd, TCIOFLUSH)) fail(&capture, "Cannot configure or reserve sensor serial port");
        }
    }
    if (capture.state != FAILED && isatty(STDIN_FILENO) && !tcgetattr(STDIN_FILENO, &original)) {
        raw = original;
        raw.c_lflag &= ~(ICANON | ECHO);
        terminal_changed = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    char command[40];
    snprintf(command, sizeof(command), "HELLO,%s", capture.token);
    if (capture.state != FAILED && !send_command(&capture, command)) fail(&capture, "Cannot send protocol greeting");
    capture.deadline = now() + capture.timeout;
    char line[LIS_LINE_SIZE];
    size_t length = 0;
    bool stdin_open = true;
    while (capture.state != FAILED && capture.state != COMPLETE) {
        if ((interrupted || (duration > 0 && capture.started > 0 && now() - capture.started >= duration))
                && capture.state != STOPPING) {
            if (capture.state != STREAMING) { fail(&capture, "Capture cancelled before readiness"); break; }
            if (!stop(&capture)) break;
        }
        if (now() >= capture.deadline) {
            fail(&capture, capture.state == HELLO ? "Protocol greeting timeout; upgrade legacy firmware if necessary" :
                capture.state == STOPPING ? "STOP confirmation timeout; capture is invalid" : "Sensor readiness or sample timeout");
            break;
        }
        struct pollfd descriptors[2] = {{capture.fd, POLLIN, 0}, {stdin_open ? STDIN_FILENO : -1, POLLIN, 0}};
        int ready = poll(descriptors, 2, 20);
        if (ready < 0) { if (errno == EINTR) continue; fail(&capture, "Serial poll failed"); break; }
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            char keys[64];
            ssize_t size = read(STDIN_FILENO, keys, sizeof(keys));
            if (size <= 0) stdin_open = false;
            for (ssize_t i = 0; i < size; ++i) if (keys[i] == 'Q' || keys[i] == 'q' || keys[i] == 3) interrupted = 1;
        }
        if (descriptors[0].revents & POLLIN) {
            char bytes[4096];
            ssize_t size = read(capture.fd, bytes, sizeof(bytes));
            if (size < 0 && errno != EAGAIN && errno != EINTR) { fail(&capture, "Serial read failed"); break; }
            for (ssize_t i = 0; i < size && capture.state != FAILED; ++i) {
                unsigned char byte = (unsigned char)bytes[i];
                if (byte == '\r') continue;
                if (byte == '\n') {
                    line[length] = 0;
                    if (capture.state == COMPLETE) { fail(&capture, "Unexpected data after DONE"); break; }
                    line_received(&capture, line);
                    length = 0;
                } else if (byte < 32 || byte > 126 || length == sizeof(line) - 1) {
                    fail(&capture, "Invalid character or overlong serial frame"); break;
                } else line[length++] = (char)byte;
            }
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) && capture.state != COMPLETE)
            fail(&capture, "Sensor disconnected");
    }
    if (capture.state != COMPLETE && capture.start_sent && capture.fd >= 0) {
        snprintf(command, sizeof(command), "STOP,%s", capture.token);
        (void)send_command(&capture, command);
    }
    if (capture.fd >= 0) close(capture.fd);
    if (terminal_changed) tcsetattr(STDIN_FILENO, TCSANOW, &original);
    if (fflush(capture.csv)) fail(&capture, "CSV flush failed");
    if (path && fsync(fileno(capture.csv))) fail(&capture, "CSV sync failed");
    if (path && fclose(capture.csv)) fail(&capture, "CSV close failed");
    if (meta_path && !metadata(meta_path, &capture)) fail(&capture, "Metadata write failed");
    free(meta_path);
    fprintf(stderr, "Captured %" PRIu64 " samples; %s\n", capture.samples,
            capture.state == COMPLETE ? "verified complete" : capture.error ? capture.error : "invalid");
    return capture.state == COMPLETE ? 0 : 2;
}

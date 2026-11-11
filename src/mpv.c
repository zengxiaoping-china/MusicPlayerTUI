#include "mpv.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_error(Mpv *mpv, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(mpv->error, sizeof(mpv->error), format, args);
    va_end(args);
}

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    size_t length;

    if (destination_size == 0)
        return;
    length = strlen(source);
    if (length >= destination_size)
        length = destination_size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

const char *mpv_error(const Mpv *mpv)
{
    return mpv->error;
}

static void cleanup_paths(Mpv *mpv)
{
    if (mpv->sockpath[0] != '\0')
        unlink(mpv->sockpath);
    if (mpv->logpath[0] != '\0')
        unlink(mpv->logpath);
    if (mpv->tmpdir[0] != '\0')
        rmdir(mpv->tmpdir);
    mpv->sockpath[0] = '\0';
    mpv->logpath[0] = '\0';
    mpv->tmpdir[0] = '\0';
}

static void read_log_tail(Mpv *mpv, char *output, size_t output_size)
{
    FILE *file;
    char buffer[2048];
    size_t length;
    char *line = NULL;
    char *cursor;

    output[0] = '\0';
    file = fopen(mpv->logpath, "r");
    if (file == NULL)
        return;

    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';

    cursor = buffer;
    while (*cursor != '\0') {
        char *end = strchr(cursor, '\n');
        if (end != NULL) {
            *end = '\0';
            if (*cursor != '\0')
                line = cursor;
            cursor = end + 1;
        } else {
            if (*cursor != '\0')
                line = cursor;
            break;
        }
    }

    if (line != NULL)
        copy_text(output, output_size, line);
}

static bool wait_for_child(Mpv *mpv, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;

    if (mpv->pid <= 0)
        return true;

    for (;;) {
        int status;
        pid_t result = waitpid(mpv->pid, &status, WNOHANG);

        if (result == mpv->pid || (result < 0 && errno == ECHILD)) {
            mpv->pid = -1;
            return true;
        }
        if (result < 0 && errno != EINTR)
            return false;
        if (now_ms() >= deadline)
            return false;
        usleep(10000);
    }
}

static void terminate_child(Mpv *mpv)
{
    if (mpv->pid <= 0)
        return;

    kill(mpv->pid, SIGTERM);
    if (!wait_for_child(mpv, 800)) {
        kill(mpv->pid, SIGKILL);
        (void)wait_for_child(mpv, 800);
    }
}

static int send_all(Mpv *mpv, const char *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = send(mpv->fd, data + offset, length - offset,
                               MSG_NOSIGNAL);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK)) {
            struct pollfd pfd = { mpv->fd, POLLOUT, 0 };
            int rc = poll(&pfd, 1, 200);
            if (rc > 0)
                continue;
            if (rc == 0)
                set_error(mpv, "写入 mpv 连接超时");
            else
                set_error(mpv, "写入 mpv 连接失败：%s", strerror(errno));
            return -1;
        }
        set_error(mpv, "写入 mpv 连接失败：%s", strerror(errno));
        return -1;
    }
    return 0;
}

static int extract_line(Mpv *mpv, char *output, size_t output_size)
{
    size_t i;
    size_t copy_length;
    size_t consumed;

    for (i = 0; i < mpv->read_length; i++) {
        if (mpv->read_buffer[i] == '\n')
            break;
    }
    if (i == mpv->read_length)
        return 0;

    copy_length = i;
    if (copy_length >= output_size)
        copy_length = output_size - 1;
    memcpy(output, mpv->read_buffer, copy_length);
    output[copy_length] = '\0';
    if (copy_length > 0 && output[copy_length - 1] == '\r')
        output[copy_length - 1] = '\0';

    consumed = i + 1;
    memmove(mpv->read_buffer, mpv->read_buffer + consumed,
            mpv->read_length - consumed);
    mpv->read_length -= consumed;
    return 1;
}

static int read_line(Mpv *mpv, char *output, size_t output_size, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;

    for (;;) {
        int remaining;
        struct pollfd pfd;
        int rc;
        char chunk[4096];
        ssize_t received;

        if (extract_line(mpv, output, output_size))
            return 1;

        remaining = (int)(deadline - now_ms());
        if (remaining <= 0)
            return 0;

        if (mpv->read_length >= sizeof(mpv->read_buffer) - 1) {
            mpv->read_length = 0;
            set_error(mpv, "mpv 响应内容过长");
            return -1;
        }

        pfd.fd = mpv->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, remaining);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            set_error(mpv, "mpv 连接轮询失败：%s", strerror(errno));
            return -1;
        }
        if (rc == 0)
            return 0;

        received = recv(mpv->fd, chunk, sizeof(chunk), 0);
        if (received > 0) {
            memcpy(mpv->read_buffer + mpv->read_length, chunk,
                   (size_t)received);
            mpv->read_length += (size_t)received;
            continue;
        }
        if (received == 0) {
            set_error(mpv, "与 mpv 的连接已断开");
            return -1;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        set_error(mpv, "读取 mpv 连接失败：%s", strerror(errno));
        return -1;
    }
}

static const char *find_json_key(const char *json, const char *key)
{
    char pattern[64];
    const char *cursor = json;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    while ((cursor = strstr(cursor, pattern)) != NULL) {
        const char *value = cursor + strlen(pattern);

        while (*value != '\0' && isspace((unsigned char)*value))
            value++;
        if (*value == ':') {
            value++;
            while (*value != '\0' && isspace((unsigned char)*value))
                value++;
            return value;
        }
        cursor += strlen(pattern);
    }
    return NULL;
}

static int parse_json_string(const char *value, char *output, size_t output_size)
{
    size_t written = 0;

    if (*value != '"')
        return -1;
    value++;

    while (*value != '\0' && *value != '"') {
        char c = *value;

        if (c == '\\') {
            value++;
            switch (*value) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
                c = '?';
                if (value[1] != '\0') value++;
                if (value[1] != '\0') value++;
                if (value[1] != '\0') value++;
                if (value[1] != '\0') value++;
                break;
            default:
                c = '?';
                break;
            }
        }

        if (written + 1 < output_size)
            output[written++] = c;
        value++;
    }

    output[written] = '\0';
    return *value == '"' ? 0 : -1;
}

static int parse_json_integer(const char *json, const char *key, long *output)
{
    const char *value = find_json_key(json, key);
    char *end = NULL;
    long parsed;

    if (value == NULL)
        return -1;

    parsed = strtol(value, &end, 10);
    if (end == value)
        return -1;
    *output = parsed;
    return 0;
}

static int wait_for_request(Mpv *mpv, int request_id, char *data,
                            size_t data_size, bool *has_data, char *error,
                            size_t error_size, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    char line[4096];

    for (;;) {
        int remaining = (int)(deadline - now_ms());
        long response_id;
        const char *value;
        char status[128];
        int rc;

        if (remaining <= 0) {
            snprintf(error, error_size, "mpv 请求超时");
            return -1;
        }

        rc = read_line(mpv, line, sizeof(line), remaining);
        if (rc < 0) {
            copy_text(error, error_size, mpv->error);
            return -1;
        }
        if (rc == 0) {
            snprintf(error, error_size, "mpv 请求超时");
            return -1;
        }

        if (parse_json_integer(line, "request_id", &response_id) != 0 ||
            response_id != request_id)
            continue;

        value = find_json_key(line, "error");
        if (value == NULL || parse_json_string(value, status,
                                               sizeof(status)) != 0) {
            snprintf(error, error_size, "mpv 响应格式错误");
            return -1;
        }
        if (strcmp(status, "success") != 0) {
            snprintf(error, error_size, "mpv: %s", status);
            return -1;
        }

        if (has_data != NULL)
            *has_data = false;
        if (data != NULL && data_size > 0)
            data[0] = '\0';

        value = find_json_key(line, "data");
        if (value != NULL) {
            if (strncmp(value, "null", 4) == 0) {
                if (has_data != NULL)
                    *has_data = false;
            } else if (*value == '"') {
                if (data != NULL && data_size > 0 &&
                    parse_json_string(value, data, data_size) == 0 &&
                    has_data != NULL)
                    *has_data = true;
            } else {
                size_t length = 0;
                while (value[length] != '\0' &&
                       value[length] != ',' && value[length] != '}' &&
                       !isspace((unsigned char)value[length]))
                    length++;
                if (data != NULL && data_size > 0) {
                    if (length >= data_size)
                        length = data_size - 1;
                    memcpy(data, value, length);
                    data[length] = '\0';
                    if (has_data != NULL)
                        *has_data = length > 0;
                }
            }
        }
        return 0;
    }
}

static int send_request(Mpv *mpv, const char *line, int request_id,
                        char *data, size_t data_size, bool *has_data,
                        char *error, size_t error_size, int timeout_ms)
{
    if (send_all(mpv, line, strlen(line)) != 0) {
        copy_text(error, error_size, mpv->error);
        return -1;
    }
    return wait_for_request(mpv, request_id, data, data_size, has_data,
                            error, error_size, timeout_ms);
}

static bool query_property(Mpv *mpv, const char *property, char *data,
                           size_t data_size, bool *has_data)
{
    int request_id = ++mpv->next_request_id;
    char error[128];
    char *line = NULL;

    if (asprintf(&line, "{\"command\":[\"get_property\",\"%s\"],"
                        "\"request_id\":%d}\n", property, request_id) < 0)
        return false;

    if (send_request(mpv, line, request_id, data, data_size, has_data,
                     error, sizeof(error), 100) != 0) {
        free(line);
        return false;
    }
    free(line);
    return true;
}

static bool query_bool(Mpv *mpv, const char *property, bool *output)
{
    char data[64];
    bool has_data = false;

    if (!query_property(mpv, property, data, sizeof(data), &has_data) ||
        !has_data)
        return false;
    if (strcmp(data, "true") == 0) {
        *output = true;
        return true;
    }
    if (strcmp(data, "false") == 0) {
        *output = false;
        return true;
    }
    return false;
}

static bool query_double(Mpv *mpv, const char *property, double *output)
{
    char data[64];
    bool has_data = false;
    char *end = NULL;
    double value;

    if (!query_property(mpv, property, data, sizeof(data), &has_data) ||
        !has_data)
        return false;

    value = strtod(data, &end);
    if (end == data)
        return false;
    *output = value;
    return true;
}

static int send_command(Mpv *mpv, const char *format, ...)
{
    va_list args;
    char *arguments = NULL;
    char *line = NULL;
    int result;

    va_start(args, format);
    if (vasprintf(&arguments, format, args) < 0) {
        va_end(args);
        set_error(mpv, "内存不足");
        return -1;
    }
    va_end(args);

    if (asprintf(&line, "{\"command\":%s}\n", arguments) < 0) {
        free(arguments);
        set_error(mpv, "内存不足");
        return -1;
    }
    free(arguments);

    result = send_all(mpv, line, strlen(line));
    free(line);
    return result;
}

static char *json_quote(const char *text)
{
    size_t length = strlen(text);
    char *output = malloc(length * 6 + 3);
    size_t read_pos = 0;
    size_t write_pos = 0;

    if (output == NULL)
        return NULL;

    output[write_pos++] = '"';
    while (read_pos < length) {
        unsigned char c = (unsigned char)text[read_pos++];

        switch (c) {
        case '"':
            output[write_pos++] = '\\';
            output[write_pos++] = '"';
            break;
        case '\\':
            output[write_pos++] = '\\';
            output[write_pos++] = '\\';
            break;
        case '\b':
            output[write_pos++] = '\\';
            output[write_pos++] = 'b';
            break;
        case '\f':
            output[write_pos++] = '\\';
            output[write_pos++] = 'f';
            break;
        case '\n':
            output[write_pos++] = '\\';
            output[write_pos++] = 'n';
            break;
        case '\r':
            output[write_pos++] = '\\';
            output[write_pos++] = 'r';
            break;
        case '\t':
            output[write_pos++] = '\\';
            output[write_pos++] = 't';
            break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                output[write_pos++] = '\\';
                output[write_pos++] = 'u';
                output[write_pos++] = '0';
                output[write_pos++] = '0';
                output[write_pos++] = hex[c >> 4];
                output[write_pos++] = hex[c & 0x0f];
            } else {
                output[write_pos++] = (char)c;
            }
            break;
        }
    }
    output[write_pos++] = '"';
    output[write_pos] = '\0';
    return output;
}

int mpv_start(Mpv *mpv)
{
    char directory_template[] = "/tmp/musicplayertui-XXXXXX";
    int log_fd;
    int null_fd;
    pid_t pid;
    int connected_fd = -1;
    int attempt;

    memset(mpv, 0, sizeof(*mpv));
    mpv->pid = -1;
    mpv->fd = -1;
    mpv->volume = 80.0;

    if (mkdtemp(directory_template) == NULL) {
        set_error(mpv, "无法创建临时目录：%s", strerror(errno));
        return -1;
    }
    snprintf(mpv->tmpdir, sizeof(mpv->tmpdir), "%s", directory_template);
    snprintf(mpv->sockpath, sizeof(mpv->sockpath), "%s/mpv.sock", mpv->tmpdir);
    snprintf(mpv->logpath, sizeof(mpv->logpath), "%s/mpv.log", mpv->tmpdir);

    if (strlen(mpv->sockpath) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        set_error(mpv, "socket 路径过长");
        cleanup_paths(mpv);
        return -1;
    }

    log_fd = open(mpv->logpath, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    null_fd = open("/dev/null", O_RDONLY);

    pid = fork();
    if (pid < 0) {
        set_error(mpv, "无法启动 mpv：%s", strerror(errno));
        if (log_fd >= 0) close(log_fd);
        if (null_fd >= 0) close(null_fd);
        cleanup_paths(mpv);
        return -1;
    }

    if (pid == 0) {
        char ipc_argument[sizeof(mpv->sockpath) + 32];
        int output_fd = open("/dev/null", O_WRONLY);

        if (null_fd >= 0)
            dup2(null_fd, STDIN_FILENO);
        if (output_fd >= 0)
            dup2(output_fd, STDOUT_FILENO);
        if (log_fd >= 0)
            dup2(log_fd, STDERR_FILENO);
        else if (output_fd >= 0)
            dup2(output_fd, STDERR_FILENO);

        if (null_fd > STDERR_FILENO)
            close(null_fd);
        if (output_fd > STDERR_FILENO)
            close(output_fd);
        if (log_fd > STDERR_FILENO)
            close(log_fd);

        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1)
            _exit(1);

        snprintf(ipc_argument, sizeof(ipc_argument), "--input-ipc-server=%s",
                 mpv->sockpath);
        execlp("mpv", "mpv",
               "--no-video",
               "--no-terminal",
               "--really-quiet",
               "--idle=yes",
               "--audio-display=no",
               "--force-window=no",
               "--loop-file=no",
               "--loop-playlist=no",
               "--keep-open=no",
               "--resume-playback=no",
               "--pause=no",
               "--volume=80",
               ipc_argument,
               (char *)NULL);

        dprintf(STDERR_FILENO, "无法执行 mpv：%s\n", strerror(errno));
        _exit(127);
    }

    if (log_fd >= 0)
        close(log_fd);
    if (null_fd >= 0)
        close(null_fd);

    mpv->pid = pid;

    for (attempt = 0; attempt < 150; attempt++) {
        struct sockaddr_un address;
        int status;
        int fd;
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            char log_line[512];
            char detail[768];

            mpv->pid = -1;
            read_log_tail(mpv, log_line, sizeof(log_line));
            if (log_line[0] != '\0')
                snprintf(detail, sizeof(detail), "mpv 在启动过程中退出：%s",
                         log_line);
            else
                snprintf(detail, sizeof(detail), "mpv 在启动过程中退出");
            set_error(mpv, "%s", detail);
            cleanup_paths(mpv);
            return -1;
        }

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            set_error(mpv, "无法创建 socket：%s", strerror(errno));
            terminate_child(mpv);
            cleanup_paths(mpv);
            return -1;
        }

        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        copy_text(address.sun_path, sizeof(address.sun_path), mpv->sockpath);

        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            connected_fd = fd;
            break;
        }
        close(fd);
        usleep(20000);
    }

    if (connected_fd < 0) {
        set_error(mpv, "等待 mpv 启动超时");
        terminate_child(mpv);
        cleanup_paths(mpv);
        return -1;
    }

    if (fcntl(connected_fd, F_SETFL, O_NONBLOCK) != 0) {
        set_error(mpv, "无法将 socket 设置为非阻塞：%s", strerror(errno));
        close(connected_fd);
        terminate_child(mpv);
        cleanup_paths(mpv);
        return -1;
    }

    mpv->fd = connected_fd;
    mpv->running = true;
    mpv->idle = true;
    mpv->paused = false;
    return 0;
}

void mpv_shutdown(Mpv *mpv)
{
    if (mpv->fd >= 0) {
        static const char quit_command[] = "{\"command\":[\"quit\"]}\n";
        (void)send_all(mpv, quit_command, sizeof(quit_command) - 1);
        close(mpv->fd);
        mpv->fd = -1;
    }

    if (mpv->pid > 0) {
        if (!wait_for_child(mpv, 500))
            terminate_child(mpv);
    }

    cleanup_paths(mpv);
    mpv->running = false;
    mpv->idle = true;
    mpv->paused = false;
}

bool mpv_is_running(Mpv *mpv)
{
    int status;
    pid_t result;

    if (mpv->pid <= 0)
        return false;

    result = waitpid(mpv->pid, &status, WNOHANG);
    if (result == mpv->pid) {
        mpv->pid = -1;
        if (mpv->fd >= 0) {
            close(mpv->fd);
            mpv->fd = -1;
        }
        mpv->running = false;
        set_error(mpv, "mpv 进程已退出");
        return false;
    }
    if (result < 0 && errno == ECHILD) {
        mpv->pid = -1;
        mpv->running = false;
        return false;
    }
    return true;
}

int mpv_load(Mpv *mpv, const char *path)
{
    char *quoted;
    char *line = NULL;
    char error[160];
    int request_id;

    if (!mpv_is_running(mpv)) {
        set_error(mpv, "mpv 未运行");
        return -1;
    }

    quoted = json_quote(path);
    if (quoted == NULL) {
        set_error(mpv, "内存不足");
        return -1;
    }

    request_id = ++mpv->next_request_id;
    if (asprintf(&line, "{\"command\":[\"loadfile\",%s,\"replace\"],"
                        "\"request_id\":%d}\n", quoted, request_id) < 0) {
        free(quoted);
        set_error(mpv, "内存不足");
        return -1;
    }
    free(quoted);

    if (send_request(mpv, line, request_id, NULL, 0, NULL,
                     error, sizeof(error), 2000) != 0) {
        free(line);
        set_error(mpv, "%s", error);
        return -1;
    }
    free(line);

    mpv->idle = false;
    mpv->paused = false;
    mpv->time_pos = 0.0;
    mpv->duration = 0.0;
    return 0;
}

int mpv_set_pause(Mpv *mpv, bool pause)
{
    if (!mpv_is_running(mpv))
        return -1;
    if (send_command(mpv, "[\"set_property\",\"pause\",%s]",
                     pause ? "true" : "false") != 0)
        return -1;
    mpv->paused = pause;
    return 0;
}

int mpv_seek_relative(Mpv *mpv, double seconds)
{
    if (!mpv_is_running(mpv))
        return -1;
    return send_command(mpv, "[\"seek\",%.3f,\"relative\"]", seconds);
}

int mpv_set_volume(Mpv *mpv, double volume)
{
    if (!mpv_is_running(mpv))
        return -1;
    if (send_command(mpv, "[\"set_property\",\"volume\",%.2f]", volume) != 0)
        return -1;
    mpv->volume = volume;
    return 0;
}

int mpv_stop(Mpv *mpv)
{
    if (!mpv_is_running(mpv))
        return -1;
    if (send_command(mpv, "[\"stop\"]") != 0)
        return -1;
    mpv->idle = true;
    mpv->paused = false;
    mpv->time_pos = 0.0;
    mpv->duration = 0.0;
    return 0;
}

int mpv_poll(Mpv *mpv)
{
    bool idle = mpv->idle;
    double value;

    if (!mpv_is_running(mpv))
        return -1;

    if (query_bool(mpv, "idle-active", &idle))
        mpv->idle = idle;

    if (!mpv->idle) {
        bool paused = mpv->paused;

        if (query_bool(mpv, "pause", &paused))
            mpv->paused = paused;
        if (query_double(mpv, "time-pos", &value))
            mpv->time_pos = value;
        if (query_double(mpv, "duration", &value))
            mpv->duration = value;
    } else {
        mpv->paused = false;
        mpv->time_pos = 0.0;
        mpv->duration = 0.0;
    }

    if (query_double(mpv, "volume", &value))
        mpv->volume = value;

    if (!mpv_is_running(mpv))
        return -1;
    return 0;
}

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_CAPACITY 32768
#define RESPONSE_CAPACITY 32768
#define CONFIG_LINE_CAPACITY 65536
#define CONFIG_FILE_CAPACITY (CONFIG_LINE_CAPACITY * 2)
#define FIELD_CAPACITY 32
#define CHANNEL_COUNT 5
#define CHANNELS_DIR "/config/channels"
#define GLOBAL_CONFIG "/config/global.conf"
#define LEGACY_SETTINGS_DIR "/config/settings"
#define APPLY_REQUEST "/run/webrtc-player/config-apply.request"
#define APPLY_RESULT "/run/webrtc-player/config-apply.result"

struct field {
    char *name;
    char *value;
};

struct http_request {
    char method[8];
    char path[128];
    char requested_with[64];
    char content_type[128];
    char *body;
    size_t content_length;
};

struct channel_config {
    char channel_name[65];
    char channel_enabled[6];
    char input_mode[32];
    char srt_url[2049];
    char srt_audio[16];
    char audio_enabled[16];
    char srt_color_mode[32];
    char video_preset[32];
    char video_bitrate[REQUEST_CAPACITY];
    char video_buffer_size[REQUEST_CAPACITY];
    char audio_bitrate[REQUEST_CAPACITY];
    char max_width[16];
    char max_height[16];
    char max_fps[16];
    char input_timeout_ms[16];
    char srt_pbkeylen[16];
    char srt_passphrase[REQUEST_CAPACITY];
};

struct config_pair {
    const char *key;
    const char *value;
};

struct file_snapshot {
    bool exists;
    char *data;
    size_t length;
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void trim_line(char *value) {
    value[strcspn(value, "\r\n")] = '\0';
}

static bool contains_line_break(const char *value) {
    return value == NULL || strchr(value, '\r') != NULL || strchr(value, '\n') != NULL;
}

static bool copy_value(char *destination, size_t capacity, const char *value) {
    size_t length;
    if (value == NULL) return false;
    length = strlen(value);
    if (length >= capacity) return false;
    memcpy(destination, value, length + 1);
    return true;
}

static bool read_single_line(const char *path, char *value, size_t capacity) {
    FILE *file = fopen(path, "r");
    char *newline;
    bool complete;
    if (file == NULL) return false;
    if (fgets(value, (int)capacity, file) == NULL) {
        bool ok = !ferror(file);
        value[0] = '\0';
        fclose(file);
        return ok;
    }
    newline = strchr(value, '\n');
    complete = newline != NULL || feof(file);
    if (newline != NULL) *newline = '\0';
    fclose(file);
    return complete && !contains_line_break(value);
}

static void fallback_value(unsigned int channel_id, const char *name, const char *fallback,
        char *value, size_t capacity) {
    const char *environment;
    char legacy_path[512];
    char legacy_value[CONFIG_LINE_CAPACITY];

    (void)copy_value(value, capacity, fallback);
    if (channel_id != 1) return;

    environment = getenv(name);
    if (environment != NULL && !contains_line_break(environment)) {
        (void)copy_value(value, capacity, environment);
    }
    snprintf(legacy_path, sizeof(legacy_path), "%s/%s", LEGACY_SETTINGS_DIR, name);
    if (read_single_line(legacy_path, legacy_value, sizeof(legacy_value))) {
        (void)copy_value(value, capacity, legacy_value);
    }
}

static void assign_channel_value(struct channel_config *config, const char *key, const char *value) {
#define ASSIGN_CHANNEL_VALUE(name, member) do { \
    if (strcmp(key, name) == 0) { (void)copy_value(config->member, sizeof(config->member), value); return; } \
} while (0)
    ASSIGN_CHANNEL_VALUE("CHANNEL_NAME", channel_name);
    ASSIGN_CHANNEL_VALUE("CHANNEL_ENABLED", channel_enabled);
    ASSIGN_CHANNEL_VALUE("INPUT_MODE", input_mode);
    ASSIGN_CHANNEL_VALUE("SRT_URL", srt_url);
    ASSIGN_CHANNEL_VALUE("SRT_AUDIO", srt_audio);
    ASSIGN_CHANNEL_VALUE("AUDIO_ENABLED", audio_enabled);
    ASSIGN_CHANNEL_VALUE("SRT_COLOR_MODE", srt_color_mode);
    ASSIGN_CHANNEL_VALUE("VIDEO_PRESET", video_preset);
    ASSIGN_CHANNEL_VALUE("VIDEO_BITRATE", video_bitrate);
    ASSIGN_CHANNEL_VALUE("VIDEO_BUFFER_SIZE", video_buffer_size);
    ASSIGN_CHANNEL_VALUE("AUDIO_BITRATE", audio_bitrate);
    ASSIGN_CHANNEL_VALUE("MAX_WIDTH", max_width);
    ASSIGN_CHANNEL_VALUE("MAX_HEIGHT", max_height);
    ASSIGN_CHANNEL_VALUE("MAX_FPS", max_fps);
    ASSIGN_CHANNEL_VALUE("INPUT_TIMEOUT_MS", input_timeout_ms);
    ASSIGN_CHANNEL_VALUE("SRT_PBKEYLEN", srt_pbkeylen);
    ASSIGN_CHANNEL_VALUE("SRT_PASSPHRASE", srt_passphrase);
#undef ASSIGN_CHANNEL_VALUE
}

static void load_channel_file(const char *path, struct channel_config *config) {
    FILE *file = fopen(path, "r");
    char line[CONFIG_LINE_CAPACITY];
    if (file == NULL) return;

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        char *separator;
        bool complete = length > 0 && line[length - 1] == '\n';
        if (complete) {
            line[--length] = '\0';
        } else if (!feof(file)) {
            int character;
            while ((character = fgetc(file)) != '\n' && character != EOF) {}
            continue;
        }
        if (contains_line_break(line)) continue;
        separator = strchr(line, '=');
        if (separator == NULL) continue;
        *separator = '\0';
        assign_channel_value(config, line, separator + 1);
    }
    fclose(file);
}

static void load_channel_config(unsigned int channel_id, struct channel_config *config) {
    char channel_name[32];
    char srt_url[128];
    char path[512];
    unsigned int srt_port = 9000 + channel_id - 1;

    memset(config, 0, sizeof(*config));
    snprintf(channel_name, sizeof(channel_name), "Channel %u", channel_id);
    snprintf(srt_url, sizeof(srt_url),
            "srt://0.0.0.0:%u?mode=listener&latency=120000", srt_port);

#define LOAD_DEFAULT(key, fallback, member) \
    fallback_value(channel_id, key, fallback, config->member, sizeof(config->member))
    LOAD_DEFAULT("CHANNEL_NAME", channel_name, channel_name);
    LOAD_DEFAULT("CHANNEL_ENABLED", "true", channel_enabled);
    LOAD_DEFAULT("INPUT_MODE", "auto", input_mode);
    LOAD_DEFAULT("SRT_URL", srt_url, srt_url);
    LOAD_DEFAULT("SRT_AUDIO", "true", srt_audio);
    LOAD_DEFAULT("AUDIO_ENABLED", "true", audio_enabled);
    LOAD_DEFAULT("SRT_COLOR_MODE", "auto", srt_color_mode);
    LOAD_DEFAULT("VIDEO_PRESET", "veryfast", video_preset);
    LOAD_DEFAULT("VIDEO_BITRATE", "6M", video_bitrate);
    LOAD_DEFAULT("VIDEO_BUFFER_SIZE", "2M", video_buffer_size);
    LOAD_DEFAULT("AUDIO_BITRATE", "128k", audio_bitrate);
    LOAD_DEFAULT("MAX_WIDTH", "1920", max_width);
    LOAD_DEFAULT("MAX_HEIGHT", "1080", max_height);
    LOAD_DEFAULT("MAX_FPS", "60", max_fps);
    LOAD_DEFAULT("INPUT_TIMEOUT_MS", "5000", input_timeout_ms);
    LOAD_DEFAULT("SRT_PBKEYLEN", "16", srt_pbkeylen);
    LOAD_DEFAULT("SRT_PASSPHRASE", "", srt_passphrase);
#undef LOAD_DEFAULT

    snprintf(path, sizeof(path), "%s/channel-%u.conf", CHANNELS_DIR, channel_id);
    load_channel_file(path, config);
}

static void load_global_config(char *public_ip, size_t capacity) {
    FILE *file;
    char line[CONFIG_LINE_CAPACITY];
    const char *environment = getenv("PUBLIC_IP");
    char legacy_path[512];
    char legacy_value[CONFIG_LINE_CAPACITY];

    (void)copy_value(public_ip, capacity, "");
    if (environment != NULL && !contains_line_break(environment)) {
        (void)copy_value(public_ip, capacity, environment);
    }
    snprintf(legacy_path, sizeof(legacy_path), "%s/PUBLIC_IP", LEGACY_SETTINGS_DIR);
    if (read_single_line(legacy_path, legacy_value, sizeof(legacy_value))) {
        (void)copy_value(public_ip, capacity, legacy_value);
    }

    file = fopen(GLOBAL_CONFIG, "r");
    if (file == NULL) return;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        char *separator;
        bool complete = length > 0 && line[length - 1] == '\n';
        if (complete) {
            line[--length] = '\0';
        } else if (!feof(file)) {
            int character;
            while ((character = fgetc(file)) != '\n' && character != EOF) {}
            continue;
        }
        if (contains_line_break(line)) continue;
        separator = strchr(line, '=');
        if (separator == NULL) continue;
        *separator = '\0';
        if (strcmp(line, "PUBLIC_IP") == 0) {
            (void)copy_value(public_ip, capacity, separator + 1);
        }
    }
    fclose(file);
}

static bool ensure_config_directories(void) {
    struct stat status;
    if (mkdir(CHANNELS_DIR, 0750) != 0 && errno != EEXIST) return false;
    return stat(CHANNELS_DIR, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool open_atomic_file(const char *path, char *temporary, size_t capacity, FILE **file) {
    int descriptor;
    int written = snprintf(temporary, capacity, "%s.tmp.XXXXXX", path);
    if (written < 0 || (size_t)written >= capacity) return false;
    descriptor = mkstemp(temporary);
    if (descriptor < 0) return false;
    if (fchmod(descriptor, 0600) != 0) {
        close(descriptor);
        (void)unlink(temporary);
        return false;
    }
    *file = fdopen(descriptor, "w");
    if (*file == NULL) {
        close(descriptor);
        (void)unlink(temporary);
        return false;
    }
    return true;
}

static bool commit_atomic_file(FILE *file, const char *temporary, const char *path, bool ok) {
    int descriptor = fileno(file);
    if (ok && fflush(file) != 0) ok = false;
    if (ok && fsync(descriptor) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temporary, path) == 0) return true;
    (void)unlink(temporary);
    return false;
}

static bool atomic_write_pairs(const char *path, const struct config_pair *pairs, size_t count) {
    char temporary[512];
    FILE *file;
    bool ok = true;
    if (!open_atomic_file(path, temporary, sizeof(temporary), &file)) return false;
    for (size_t index = 0; index < count; index++) {
        if (contains_line_break(pairs[index].key) || contains_line_break(pairs[index].value) ||
                fprintf(file, "%s=%s\n", pairs[index].key, pairs[index].value) < 0) {
            ok = false;
            break;
        }
    }
    return commit_atomic_file(file, temporary, path, ok);
}

static bool atomic_write_bytes(const char *path, const char *data, size_t length) {
    char temporary[512];
    FILE *file;
    bool ok;
    if (!open_atomic_file(path, temporary, sizeof(temporary), &file)) return false;
    ok = length == 0 || fwrite(data, 1, length, file) == length;
    return commit_atomic_file(file, temporary, path, ok);
}

static bool snapshot_file(const char *path, struct file_snapshot *snapshot) {
    struct stat status;
    FILE *file;
    size_t length;

    memset(snapshot, 0, sizeof(*snapshot));
    if (stat(path, &status) != 0) return errno == ENOENT;
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || status.st_size > CONFIG_FILE_CAPACITY ||
            (unsigned long long)status.st_size > (unsigned long long)SIZE_MAX) return false;
    length = (size_t)status.st_size;
    snapshot->data = malloc(length == 0 ? 1 : length);
    if (snapshot->data == NULL) return false;
    file = fopen(path, "rb");
    if (file == NULL) {
        free(snapshot->data);
        snapshot->data = NULL;
        return false;
    }
    if (length > 0 && fread(snapshot->data, 1, length, file) != length) {
        fclose(file);
        free(snapshot->data);
        snapshot->data = NULL;
        return false;
    }
    if (fgetc(file) != EOF || ferror(file)) {
        fclose(file);
        free(snapshot->data);
        snapshot->data = NULL;
        return false;
    }
    if (fclose(file) != 0) {
        free(snapshot->data);
        snapshot->data = NULL;
        return false;
    }
    snapshot->exists = true;
    snapshot->length = length;
    return true;
}

static bool restore_snapshot(const char *path, const struct file_snapshot *snapshot) {
    if (snapshot->exists) return atomic_write_bytes(path, snapshot->data, snapshot->length);
    return unlink(path) == 0 || errno == ENOENT;
}

static void free_snapshot(struct file_snapshot *snapshot) {
    free(snapshot->data);
    snapshot->data = NULL;
}

static bool append_literal(char *output, size_t capacity, size_t *length, const char *value) {
    size_t value_length = strlen(value);
    if (*length >= capacity || value_length >= capacity - *length) return false;
    memcpy(output + *length, value, value_length + 1);
    *length += value_length;
    return true;
}

static bool append_json_string(char *output, size_t capacity, size_t *length, const char *value) {
    if (*length + 2 >= capacity) return false;
    output[(*length)++] = '"';
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        const char *escape = NULL;
        char unicode_escape[7];
        if (*cursor == '"') escape = "\\\"";
        else if (*cursor == '\\') escape = "\\\\";
        else if (*cursor == '\b') escape = "\\b";
        else if (*cursor == '\f') escape = "\\f";
        else if (*cursor == '\n') escape = "\\n";
        else if (*cursor == '\r') escape = "\\r";
        else if (*cursor == '\t') escape = "\\t";
        else if (*cursor < 0x20 || *cursor >= 0x7f) {
            snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *cursor);
            escape = unicode_escape;
        }
        if (escape != NULL) {
            size_t escape_length = strlen(escape);
            if (escape_length >= capacity - *length) return false;
            memcpy(output + *length, escape, escape_length);
            *length += escape_length;
        } else {
            if (*length + 1 >= capacity) return false;
            output[(*length)++] = (char)*cursor;
        }
    }
    if (*length + 1 >= capacity) return false;
    output[(*length)++] = '"';
    output[*length] = '\0';
    return true;
}

static bool append_json_member(char *output, size_t capacity, size_t *length,
        const char *name, const char *value, bool comma) {
    return append_json_string(output, capacity, length, name) &&
            append_literal(output, capacity, length, ":") &&
            append_json_string(output, capacity, length, value) &&
            (!comma || append_literal(output, capacity, length, ","));
}

static bool send_all(int client, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(client, data + sent, length - sent, MSG_NOSIGNAL);
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static void send_response(int client, int status, const char *body) {
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
            status == 401 ? "Unauthorized" : status == 404 ? "Not Found" :
            status == 405 ? "Method Not Allowed" : "Internal Server Error";
    char header[512];
    int header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
            "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
            status, reason, strlen(body));
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) return;
    if (!send_all(client, header, (size_t)header_length)) return;
    (void)send_all(client, body, strlen(body));
}

static bool header_value(const char *line, const char *name, char *output, size_t capacity) {
    size_t name_length = strlen(name);
    size_t line_length = strlen(line);
    const char *value;
    if (line_length <= name_length || strncasecmp(line, name, name_length) != 0 ||
            line[name_length] != ':') return false;
    value = line + name_length + 1;
    while (*value == ' ' || *value == '\t') value++;
    if (!copy_value(output, capacity, value)) return false;
    trim_line(output);
    return true;
}

static bool parse_content_length(const char *value, size_t *length) {
    char *end = NULL;
    unsigned long long number;
    if (value == NULL || *value == '\0' || *value == '-') return false;
    errno = 0;
    number = strtoull(value, &end, 10);
    if (errno != 0 || *end != '\0' || number >= REQUEST_CAPACITY) return false;
    *length = (size_t)number;
    return true;
}

static bool receive_request(int client, char *buffer, struct http_request *request) {
    size_t total = 0;
    char *header_end = NULL;
    size_t body_offset;
    char *line;
    char *save = NULL;
    char content_length[32] = "0";

    memset(request, 0, sizeof(*request));
    while (total + 1 < REQUEST_CAPACITY) {
        ssize_t received = recv(client, buffer + total, REQUEST_CAPACITY - total - 1, 0);
        if (received <= 0) return false;
        total += (size_t)received;
        buffer[total] = '\0';
        header_end = strstr(buffer, "\r\n\r\n");
        if (header_end != NULL) break;
    }
    if (header_end == NULL) return false;
    body_offset = (size_t)(header_end - buffer) + 4;
    *header_end = '\0';

    line = strtok_r(buffer, "\r\n", &save);
    if (line == NULL || sscanf(line, "%7s %127s", request->method, request->path) != 2) return false;
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (header_value(line, "X-Requested-With", request->requested_with,
                sizeof(request->requested_with))) continue;
        if (header_value(line, "Content-Type", request->content_type,
                sizeof(request->content_type))) continue;
        (void)header_value(line, "Content-Length", content_length, sizeof(content_length));
    }
    if (!parse_content_length(content_length, &request->content_length) ||
            request->content_length >= REQUEST_CAPACITY - body_offset) return false;
    while (total - body_offset < request->content_length) {
        ssize_t received = recv(client, buffer + total, REQUEST_CAPACITY - total - 1, 0);
        if (received <= 0) return false;
        total += (size_t)received;
    }
    request->body = buffer + body_offset;
    request->body[request->content_length] = '\0';
    return true;
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static bool url_decode(char *value) {
    char *source = value;
    char *destination = value;
    while (*source != '\0') {
        unsigned char decoded;
        if (*source == '+') {
            *destination++ = ' ';
            source++;
        } else if (*source == '%') {
            int high;
            int low;
            if (source[1] == '\0' || source[2] == '\0' ||
                    (high = hex_value(source[1])) < 0 || (low = hex_value(source[2])) < 0) return false;
            decoded = (unsigned char)((high << 4) | low);
            if (decoded == '\0') return false;
            *destination++ = (char)decoded;
            source += 3;
        } else {
            *destination++ = *source++;
        }
    }
    *destination = '\0';
    return true;
}

static bool parse_fields(char *body, struct field *fields, size_t *count) {
    char *pair;
    char *save = NULL;
    *count = 0;
    for (pair = strtok_r(body, "&", &save); pair != NULL; pair = strtok_r(NULL, "&", &save)) {
        char *separator;
        if (*count >= FIELD_CAPACITY) return false;
        separator = strchr(pair, '=');
        if (separator == NULL) return false;
        *separator = '\0';
        fields[*count].name = pair;
        fields[*count].value = separator + 1;
        if (!url_decode(fields[*count].name) || !url_decode(fields[*count].value)) return false;
        for (size_t index = 0; index < *count; index++) {
            if (strcmp(fields[index].name, fields[*count].name) == 0) return false;
        }
        (*count)++;
    }
    return *count > 0;
}

static const char *field_value(const struct field *fields, size_t count, const char *name) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(fields[index].name, name) == 0) return fields[index].value;
    }
    return NULL;
}

static bool one_of(const char *value, const char *const *choices) {
    if (value == NULL) return false;
    for (size_t index = 0; choices[index] != NULL; index++) {
        if (strcmp(value, choices[index]) == 0) return true;
    }
    return false;
}

static bool integer_in_range(const char *value, long minimum, long maximum) {
    char *end = NULL;
    long number;
    if (value == NULL || *value == '\0') return false;
    errno = 0;
    number = strtol(value, &end, 10);
    return errno == 0 && *end == '\0' && number >= minimum && number <= maximum;
}

static bool valid_bitrate(const char *value) {
    size_t length;
    if (value == NULL || *value < '1' || *value > '9') return false;
    length = strlen(value);
    for (size_t index = 0; index < length; index++) {
        if (isdigit((unsigned char)value[index])) continue;
        return index == length - 1 && (value[index] == 'k' || value[index] == 'K' || value[index] == 'M');
    }
    return true;
}

static bool valid_public_ip(const char *value) {
    unsigned char address[sizeof(struct in6_addr)];
    if (value == NULL) return false;
    return *value == '\0' || inet_pton(AF_INET, value, address) == 1 ||
            inet_pton(AF_INET6, value, address) == 1;
}

static bool valid_srt_url(const char *value) {
    return value != NULL && strncmp(value, "srt://", 6) == 0 && strlen(value) <= 2048 &&
            !contains_line_break(value);
}

static bool valid_channel_name(const char *value) {
    size_t length;
    if (value == NULL) return false;
    length = strlen(value);
    if (length < 1 || length > 64) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)value[index];
        if (character < 0x20 || character > 0x7e) return false;
    }
    return true;
}

static bool valid_form_content_type(const char *value) {
    static const char expected[] = "application/x-www-form-urlencoded";
    size_t length = sizeof(expected) - 1;
    return strncasecmp(value, expected, length) == 0 &&
            (value[length] == '\0' || value[length] == ';');
}

static bool result_generation_matches(const char *result_generation, const char *generation) {
    char compatibility[128];
    int written;
    if (strcmp(result_generation, generation) == 0) return true;
    written = snprintf(compatibility, sizeof(compatibility), "generation=%s", generation);
    return written >= 0 && (size_t)written < sizeof(compatibility) &&
            strcmp(result_generation, compatibility) == 0;
}

static bool request_apply(unsigned int channel_id, bool global_changed) {
    char generation[64];
    char channel[16];
    char line[256];
    struct timespec now;
    struct config_pair request_pairs[3];

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return false;
    snprintf(generation, sizeof(generation), "%lld-%ld-%ld", (long long)now.tv_sec,
            now.tv_nsec, (long)getpid());
    snprintf(channel, sizeof(channel), "%u", channel_id);
    request_pairs[0] = (struct config_pair){"generation", generation};
    request_pairs[1] = (struct config_pair){"channel", channel};
    request_pairs[2] = (struct config_pair){"global", global_changed ? "true" : "false"};
    if (!atomic_write_pairs(APPLY_REQUEST, request_pairs, 3)) return false;

    for (unsigned int attempt = 0; attempt < 200; attempt++) {
        FILE *result = fopen(APPLY_RESULT, "r");
        char result_generation[128] = "";
        char result_status[32] = "";
        if (result != NULL) {
            while (fgets(line, sizeof(line), result) != NULL) {
                char *separator;
                trim_line(line);
                if (contains_line_break(line)) continue;
                separator = strchr(line, '=');
                if (separator == NULL) continue;
                *separator = '\0';
                if (strcmp(line, "generation") == 0) {
                    (void)copy_value(result_generation, sizeof(result_generation), separator + 1);
                } else if (strcmp(line, "status") == 0) {
                    (void)copy_value(result_status, sizeof(result_status), separator + 1);
                }
            }
            fclose(result);
            if (result_generation_matches(result_generation, generation)) {
                return strcmp(result_status, "ok") == 0;
            }
        }
        usleep(100000);
    }
    return false;
}

static bool append_channel_json(char *response, size_t capacity, size_t *length,
        unsigned int channel_id, const struct channel_config *config) {
    char fixed[512];
    unsigned int base = 5004 + 4 * (channel_id - 1);
    unsigned int srt_port = 9000 + channel_id - 1;
    int fixed_length;

    if (!append_literal(response, capacity, length, "{\"id\":")) return false;
    fixed_length = snprintf(fixed, sizeof(fixed), "%u", channel_id);
    if (fixed_length < 0 || (size_t)fixed_length >= sizeof(fixed) ||
            !append_literal(response, capacity, length, fixed) ||
            !append_literal(response, capacity, length, ",\"name\":") ||
            !append_json_string(response, capacity, length, config->channel_name) ||
            !append_literal(response, capacity, length, ",\"enabled\":") ||
            !append_literal(response, capacity, length,
                    strcmp(config->channel_enabled, "true") == 0 ? "true" : "false") ||
            !append_literal(response, capacity, length, ",\"passphraseConfigured\":") ||
            !append_literal(response, capacity, length,
                    config->srt_passphrase[0] != '\0' ? "true" : "false") ||
            !append_literal(response, capacity, length, ",\"values\":{")) return false;

    if (!append_json_member(response, capacity, length, "INPUT_MODE", config->input_mode, true) ||
            !append_json_member(response, capacity, length, "SRT_URL", config->srt_url, true) ||
            !append_json_member(response, capacity, length, "SRT_AUDIO", config->srt_audio, true) ||
            !append_json_member(response, capacity, length, "AUDIO_ENABLED", config->audio_enabled, true) ||
            !append_json_member(response, capacity, length, "SRT_COLOR_MODE", config->srt_color_mode, true) ||
            !append_json_member(response, capacity, length, "VIDEO_PRESET", config->video_preset, true) ||
            !append_json_member(response, capacity, length, "VIDEO_BITRATE", config->video_bitrate, true) ||
            !append_json_member(response, capacity, length, "VIDEO_BUFFER_SIZE", config->video_buffer_size, true) ||
            !append_json_member(response, capacity, length, "AUDIO_BITRATE", config->audio_bitrate, true) ||
            !append_json_member(response, capacity, length, "MAX_WIDTH", config->max_width, true) ||
            !append_json_member(response, capacity, length, "MAX_HEIGHT", config->max_height, true) ||
            !append_json_member(response, capacity, length, "MAX_FPS", config->max_fps, true) ||
            !append_json_member(response, capacity, length, "INPUT_TIMEOUT_MS", config->input_timeout_ms, true) ||
            !append_json_member(response, capacity, length, "SRT_PBKEYLEN", config->srt_pbkeylen, false)) return false;

    fixed_length = snprintf(fixed, sizeof(fixed),
            "},\"fixed\":{\"STREAM_ID\":%u,\"VIDEO_PORT\":%u,\"AUDIO_PORT\":%u,"
            "\"VIDEO_RTCP_PORT\":%u,\"AUDIO_RTCP_PORT\":%u,\"SRT_PORT\":%u,"
            "\"HTTP_PORT\":8088,\"ICE_PORTS\":\"20000-20100/udp\"}}",
            channel_id, base, base + 1, base + 2, base + 3, srt_port);
    return fixed_length >= 0 && (size_t)fixed_length < sizeof(fixed) &&
            append_literal(response, capacity, length, fixed);
}

static void handle_get(int client) {
    char response[RESPONSE_CAPACITY];
    char public_ip[128];
    struct channel_config config;
    size_t length = 0;
    bool ok;

    response[0] = '\0';
    load_global_config(public_ip, sizeof(public_ip));
    ok = append_literal(response, sizeof(response), &length,
            "{\"version\":2,\"global\":{\"PUBLIC_IP\":") &&
            append_json_string(response, sizeof(response), &length, public_ip) &&
            append_literal(response, sizeof(response), &length, "},\"channels\":[");
    for (unsigned int channel_id = 1; ok && channel_id <= CHANNEL_COUNT; channel_id++) {
        load_channel_config(channel_id, &config);
        if (channel_id > 1) ok = append_literal(response, sizeof(response), &length, ",");
        if (ok) ok = append_channel_json(response, sizeof(response), &length, channel_id, &config);
    }
    if (ok) ok = append_literal(response, sizeof(response), &length, "]}");
    if (!ok) {
        send_response(client, 500, "{\"error\":\"Response too large\"}");
        return;
    }
    send_response(client, 200, response);
}

static void handle_global_post(int client, struct field *fields, size_t count) {
    const char *public_ip = field_value(fields, count, "PUBLIC_IP");
    struct config_pair global_pairs[1];
    struct file_snapshot global_snapshot;
    char old_public_ip[128];

    if (!valid_public_ip(public_ip)) {
        send_response(client, 400, "{\"error\":\"Global settings are invalid\"}");
        return;
    }
    load_global_config(old_public_ip, sizeof(old_public_ip));
    if (strcmp(old_public_ip, public_ip) == 0) {
        send_response(client, 200,
                "{\"ok\":true,\"scope\":\"global\",\"message\":\"Global settings unchanged\"}");
        return;
    }
    if (!ensure_config_directories()) {
        send_response(client, 500, "{\"error\":\"Could not prepare configuration storage\"}");
        return;
    }
    if (!snapshot_file(GLOBAL_CONFIG, &global_snapshot)) {
        send_response(client, 500, "{\"error\":\"Could not read existing global settings\"}");
        return;
    }
    global_pairs[0] = (struct config_pair){"PUBLIC_IP", public_ip};
    if (!atomic_write_pairs(GLOBAL_CONFIG, global_pairs, 1)) {
        bool restored = restore_snapshot(GLOBAL_CONFIG, &global_snapshot);
        free_snapshot(&global_snapshot);
        send_response(client, 500, restored ?
                "{\"error\":\"Could not persist global settings\"}" :
                "{\"error\":\"Could not persist or restore global settings\"}");
        return;
    }
    if (!request_apply(0, true)) {
        bool restored = restore_snapshot(GLOBAL_CONFIG, &global_snapshot);
        free_snapshot(&global_snapshot);
        send_response(client, 500, restored ?
                "{\"error\":\"Services could not apply global settings; previous settings were restored\"}" :
                "{\"error\":\"Services could not apply global settings and rollback was incomplete\"}");
        return;
    }
    free_snapshot(&global_snapshot);
    send_response(client, 200,
            "{\"ok\":true,\"scope\":\"global\",\"message\":\"Global settings applied\"}");
}

static void handle_post(int client, struct field *fields, size_t count) {
    static const char *const modes[] = {"auto", "rtp", "srt", NULL};
    static const char *const booleans[] = {"true", "false", NULL};
    static const char *const colors[] = {"auto", "fast", "hdr-to-sdr", NULL};
    static const char *const presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", NULL};
    static const char *const passphrase_actions[] = {"keep", "set", "clear", NULL};
    const char *scope = field_value(fields, count, "scope");
    const char *channel_id_value = field_value(fields, count, "CHANNEL_ID");
    const char *channel_name = field_value(fields, count, "CHANNEL_NAME");
    const char *channel_enabled = field_value(fields, count, "CHANNEL_ENABLED");
    const char *mode = field_value(fields, count, "INPUT_MODE");
    const char *srt_url = field_value(fields, count, "SRT_URL");
    const char *srt_audio = field_value(fields, count, "SRT_AUDIO");
    const char *audio_enabled = field_value(fields, count, "AUDIO_ENABLED");
    const char *color_mode = field_value(fields, count, "SRT_COLOR_MODE");
    const char *video_preset = field_value(fields, count, "VIDEO_PRESET");
    const char *video_bitrate = field_value(fields, count, "VIDEO_BITRATE");
    const char *video_buffer_size = field_value(fields, count, "VIDEO_BUFFER_SIZE");
    const char *audio_bitrate = field_value(fields, count, "AUDIO_BITRATE");
    const char *max_width = field_value(fields, count, "MAX_WIDTH");
    const char *max_height = field_value(fields, count, "MAX_HEIGHT");
    const char *max_fps = field_value(fields, count, "MAX_FPS");
    const char *timeout = field_value(fields, count, "INPUT_TIMEOUT_MS");
    const char *pbkeylen = field_value(fields, count, "SRT_PBKEYLEN");
    const char *passphrase_action = field_value(fields, count, "passphraseAction");
    const char *passphrase = field_value(fields, count, "SRT_PASSPHRASE");
    unsigned int channel_id;
    struct channel_config old_config;
    struct config_pair channel_pairs[17];
    struct file_snapshot channel_snapshot;
    const char *selected_passphrase;
    char channel_path[512];
    char success_response[128];

    for (size_t index = 0; index < count; index++) {
        if (contains_line_break(fields[index].value)) {
            send_response(client, 400, "{\"error\":\"One or more settings are invalid\"}");
            return;
        }
    }
    if (scope != NULL && strcmp(scope, "global") == 0) {
        handle_global_post(client, fields, count);
        return;
    }
    if (scope != NULL && strcmp(scope, "channel") != 0) {
        send_response(client, 400, "{\"error\":\"Configuration scope is invalid\"}");
        return;
    }
    if (!integer_in_range(channel_id_value, 1, CHANNEL_COUNT) || !valid_channel_name(channel_name) ||
            !one_of(channel_enabled, booleans) || !one_of(mode, modes) || !valid_srt_url(srt_url) ||
            !one_of(srt_audio, booleans) || !one_of(audio_enabled, booleans) ||
            !one_of(color_mode, colors) || !one_of(video_preset, presets) ||
            !valid_bitrate(video_bitrate) || !valid_bitrate(video_buffer_size) ||
            !valid_bitrate(audio_bitrate) ||
            !integer_in_range(max_width, 16, 8192) || !integer_in_range(max_height, 16, 8192) ||
            !integer_in_range(max_fps, 1, 60) || !integer_in_range(timeout, 1000, 60000) ||
            !(pbkeylen != NULL && (strcmp(pbkeylen, "16") == 0 || strcmp(pbkeylen, "24") == 0 ||
                    strcmp(pbkeylen, "32") == 0)) ||
            !one_of(passphrase_action, passphrase_actions) || passphrase == NULL ||
            (strcmp(passphrase_action, "set") == 0 &&
                    (strlen(passphrase) < 10 || strlen(passphrase) > 79))) {
        send_response(client, 400, "{\"error\":\"One or more settings are invalid\"}");
        return;
    }

    channel_id = (unsigned int)strtoul(channel_id_value, NULL, 10);
    load_channel_config(channel_id, &old_config);
    selected_passphrase = strcmp(passphrase_action, "set") == 0 ? passphrase :
            strcmp(passphrase_action, "clear") == 0 ? "" : old_config.srt_passphrase;

    channel_pairs[0] = (struct config_pair){"CHANNEL_NAME", channel_name};
    channel_pairs[1] = (struct config_pair){"CHANNEL_ENABLED", channel_enabled};
    channel_pairs[2] = (struct config_pair){"INPUT_MODE", mode};
    channel_pairs[3] = (struct config_pair){"SRT_URL", srt_url};
    channel_pairs[4] = (struct config_pair){"SRT_AUDIO", srt_audio};
    channel_pairs[5] = (struct config_pair){"AUDIO_ENABLED", audio_enabled};
    channel_pairs[6] = (struct config_pair){"SRT_COLOR_MODE", color_mode};
    channel_pairs[7] = (struct config_pair){"VIDEO_PRESET", video_preset};
    channel_pairs[8] = (struct config_pair){"VIDEO_BITRATE", video_bitrate};
    channel_pairs[9] = (struct config_pair){"VIDEO_BUFFER_SIZE", video_buffer_size};
    channel_pairs[10] = (struct config_pair){"AUDIO_BITRATE", audio_bitrate};
    channel_pairs[11] = (struct config_pair){"MAX_WIDTH", max_width};
    channel_pairs[12] = (struct config_pair){"MAX_HEIGHT", max_height};
    channel_pairs[13] = (struct config_pair){"MAX_FPS", max_fps};
    channel_pairs[14] = (struct config_pair){"INPUT_TIMEOUT_MS", timeout};
    channel_pairs[15] = (struct config_pair){"SRT_PBKEYLEN", pbkeylen};
    channel_pairs[16] = (struct config_pair){"SRT_PASSPHRASE", selected_passphrase};

    if (!ensure_config_directories()) {
        send_response(client, 500, "{\"error\":\"Could not prepare configuration storage\"}");
        return;
    }
    snprintf(channel_path, sizeof(channel_path), "%s/channel-%u.conf", CHANNELS_DIR, channel_id);
    if (!snapshot_file(channel_path, &channel_snapshot)) {
        send_response(client, 500, "{\"error\":\"Could not read existing channel settings\"}");
        return;
    }
    if (!atomic_write_pairs(channel_path, channel_pairs, 17)) {
        bool restored = restore_snapshot(channel_path, &channel_snapshot);
        free_snapshot(&channel_snapshot);
        send_response(client, 500, restored ?
                "{\"error\":\"Could not persist settings\"}" :
                "{\"error\":\"Could not persist or restore settings\"}");
        return;
    }

    if (!request_apply(channel_id, false)) {
        bool channel_restored = restore_snapshot(channel_path, &channel_snapshot);
        free_snapshot(&channel_snapshot);
        send_response(client, 500, channel_restored ?
                "{\"error\":\"Services could not apply settings; previous settings were restored\"}" :
                "{\"error\":\"Services could not apply settings and rollback was incomplete\"}");
        return;
    }

    free_snapshot(&channel_snapshot);
    snprintf(success_response, sizeof(success_response),
            "{\"ok\":true,\"channel\":%u,\"message\":\"Settings applied\"}", channel_id);
    send_response(client, 200, success_response);
}

static void handle_client(int client) {
    char buffer[REQUEST_CAPACITY];
    struct http_request request;
    struct field fields[FIELD_CAPACITY];
    size_t field_count;

    if (!receive_request(client, buffer, &request)) {
        send_response(client, 400, "{\"error\":\"Invalid request\"}");
        return;
    }
    if (strcmp(request.path, "/config") != 0) {
        send_response(client, 404, "{\"error\":\"Not found\"}");
        return;
    }
    if (strcmp(request.method, "GET") == 0) {
        handle_get(client);
        return;
    }
    if (strcmp(request.method, "POST") != 0) {
        send_response(client, 405, "{\"error\":\"Method not allowed\"}");
        return;
    }
    if (strcmp(request.requested_with, "WebRTC-Player") != 0) {
        send_response(client, 401,
                "{\"error\":\"Configuration requests must come from the application UI\"}");
        return;
    }
    if (!valid_form_content_type(request.content_type)) {
        send_response(client, 400, "{\"error\":\"Configuration must be form encoded\"}");
        return;
    }
    if (!parse_fields(request.body, fields, &field_count)) {
        send_response(client, 400, "{\"error\":\"Invalid settings payload\"}");
        return;
    }
    handle_post(client, fields, field_count);
}

int main(void) {
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int reuse = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(8090),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (server < 0 || setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
            bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(server, 16) != 0) {
        fprintf(stderr, "[config-api] startup failed: %s\n", strerror(errno));
        if (server >= 0) close(server);
        return EXIT_FAILURE;
    }
    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    printf("[config-api] listening on 127.0.0.1:8090\n");
    fflush(stdout);

    while (running) {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[config-api] accept failed: %s\n", strerror(errno));
            break;
        }
        handle_client(client);
        close(client);
    }
    close(server);
    return EXIT_SUCCESS;
}

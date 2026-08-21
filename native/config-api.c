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
#include <sys/wait.h>
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
#define NETWORK_JSON "/run/webrtc-player/network.json"
#define NETWORK_INFO "/usr/local/bin/network-info"
#define NETWORK_JSON_CAPACITY 16384

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
    char video_port[16];
    char audio_port[16];
    char video_rtcp_port[16];
    char audio_rtcp_port[16];
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

struct global_config {
    char public_ip[128];
    char management_interface[64];
    char ingest_interface[64];
    char webrtc_interface[64];
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

struct srt_endpoint {
    unsigned int port;
    bool local_port;
};

struct udp_reservation {
    unsigned int port;
    unsigned int channel_id;
    const char *field;
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
    ASSIGN_CHANNEL_VALUE("VIDEO_PORT", video_port);
    ASSIGN_CHANNEL_VALUE("AUDIO_PORT", audio_port);
    ASSIGN_CHANNEL_VALUE("VIDEO_RTCP_PORT", video_rtcp_port);
    ASSIGN_CHANNEL_VALUE("AUDIO_RTCP_PORT", audio_rtcp_port);
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

static void normalize_legacy_srt_listener(char *value, size_t capacity) {
    const char *query;
    const char *cursor;
    char normalized[2049];
    bool local_mode = false;
    int written;

    if (strncmp(value, "srt://:", 7) != 0 || (query = strchr(value, '?')) == NULL) return;
    cursor = query + 1;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '&');
        size_t length;
        if (end == NULL) end = value + strlen(value);
        length = (size_t)(end - cursor);
        if ((length == 13 && memcmp(cursor, "mode=listener", 13) == 0) ||
                (length == 15 && memcmp(cursor, "mode=rendezvous", 15) == 0)) {
            local_mode = true;
            break;
        }
        if (*end == '\0') break;
        cursor = end + 1;
    }
    if (!local_mode) return;
    written = snprintf(normalized, sizeof(normalized), "srt://0.0.0.0%s", value + 6);
    if (written < 0 || (size_t)written >= sizeof(normalized)) return;
    (void)copy_value(value, capacity, normalized);
}

static void load_channel_config(unsigned int channel_id, struct channel_config *config) {
    char channel_name[32];
    char srt_url[128];
    char video_port[16];
    char audio_port[16];
    char video_rtcp_port[16];
    char audio_rtcp_port[16];
    char path[512];
    unsigned int base = 5004 + 4 * (channel_id - 1);
    unsigned int srt_port = 9000 + channel_id - 1;

    memset(config, 0, sizeof(*config));
    snprintf(channel_name, sizeof(channel_name), "Channel %u", channel_id);
    snprintf(srt_url, sizeof(srt_url),
            "srt://0.0.0.0:%u?mode=listener&latency=120000", srt_port);
    snprintf(video_port, sizeof(video_port), "%u", base);
    snprintf(audio_port, sizeof(audio_port), "%u", base + 1);
    snprintf(video_rtcp_port, sizeof(video_rtcp_port), "%u", base + 2);
    snprintf(audio_rtcp_port, sizeof(audio_rtcp_port), "%u", base + 3);

#define LOAD_DEFAULT(key, fallback, member) \
    fallback_value(channel_id, key, fallback, config->member, sizeof(config->member))
    LOAD_DEFAULT("CHANNEL_NAME", channel_name, channel_name);
    LOAD_DEFAULT("CHANNEL_ENABLED", "true", channel_enabled);
    LOAD_DEFAULT("INPUT_MODE", "auto", input_mode);
    LOAD_DEFAULT("SRT_URL", srt_url, srt_url);
    LOAD_DEFAULT("SRT_AUDIO", "true", srt_audio);
    LOAD_DEFAULT("AUDIO_ENABLED", "true", audio_enabled);
    LOAD_DEFAULT("VIDEO_PORT", video_port, video_port);
    LOAD_DEFAULT("AUDIO_PORT", audio_port, audio_port);
    LOAD_DEFAULT("VIDEO_RTCP_PORT", video_rtcp_port, video_rtcp_port);
    LOAD_DEFAULT("AUDIO_RTCP_PORT", audio_rtcp_port, audio_rtcp_port);
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
    normalize_legacy_srt_listener(config->srt_url, sizeof(config->srt_url));
}

static void assign_global_value(struct global_config *config, const char *key, const char *value) {
    if (strcmp(key, "PUBLIC_IP") == 0) {
        (void)copy_value(config->public_ip, sizeof(config->public_ip), value);
    } else if (strcmp(key, "MANAGEMENT_INTERFACE") == 0) {
        (void)copy_value(config->management_interface, sizeof(config->management_interface), value);
    } else if (strcmp(key, "INGEST_INTERFACE") == 0) {
        (void)copy_value(config->ingest_interface, sizeof(config->ingest_interface), value);
    } else if (strcmp(key, "WEBRTC_INTERFACE") == 0) {
        (void)copy_value(config->webrtc_interface, sizeof(config->webrtc_interface), value);
    }
}

static void load_global_config(struct global_config *config) {
    FILE *file;
    char line[CONFIG_LINE_CAPACITY];
    char legacy_path[512];
    char legacy_value[CONFIG_LINE_CAPACITY];
    static const char *const names[] = {
        "PUBLIC_IP", "MANAGEMENT_INTERFACE", "INGEST_INTERFACE", "WEBRTC_INTERFACE"
    };

    memset(config, 0, sizeof(*config));
    (void)copy_value(config->management_interface, sizeof(config->management_interface), "auto");
    (void)copy_value(config->ingest_interface, sizeof(config->ingest_interface), "auto");
    (void)copy_value(config->webrtc_interface, sizeof(config->webrtc_interface), "auto");
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        const char *environment = getenv(names[index]);
        if (environment != NULL && !contains_line_break(environment)) {
            assign_global_value(config, names[index], environment);
        }
    }
    snprintf(legacy_path, sizeof(legacy_path), "%s/PUBLIC_IP", LEGACY_SETTINGS_DIR);
    if (read_single_line(legacy_path, legacy_value, sizeof(legacy_value))) {
        (void)copy_value(config->public_ip, sizeof(config->public_ip), legacy_value);
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
        assign_global_value(config, line, separator + 1);
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
    struct in_addr address;
    if (value == NULL) return false;
    return *value == '\0' || inet_pton(AF_INET, value, &address) == 1;
}

static bool valid_interface_selector(const char *value) {
    pid_t child;
    int status;

    if (value == NULL || contains_line_break(value) || strlen(value) >= 64) return false;
    child = fork();
    if (child < 0) return false;
    if (child == 0) {
        int null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_descriptor >= 0) {
            (void)dup2(null_descriptor, STDOUT_FILENO);
            (void)dup2(null_descriptor, STDERR_FILENO);
            close(null_descriptor);
        }
        execl(NETWORK_INFO, NETWORK_INFO, "validate", value, (char *)NULL);
        _exit(127);
    }
    do {
        if (waitpid(child, &status, 0) >= 0) break;
        if (errno != EINTR) return false;
    } while (true);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool load_network_json(char *json, size_t capacity) {
    int descriptor = open(NETWORK_JSON, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat status;
    size_t length = 0;
    size_t start = 0;
    size_t end;

    if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
            status.st_uid != 0 || status.st_size < 2 || (unsigned long long)status.st_size >= capacity) {
        if (descriptor >= 0) close(descriptor);
        return false;
    }
    while (length < (size_t)status.st_size) {
        ssize_t count = read(descriptor, json + length, (size_t)status.st_size - length);
        if (count <= 0) {
            close(descriptor);
            return false;
        }
        length += (size_t)count;
    }
    if (close(descriptor) != 0) return false;
    json[length] = '\0';
    while (start < length && isspace((unsigned char)json[start])) start++;
    end = length;
    while (end > start && isspace((unsigned char)json[end - 1])) end--;
    if (end - start < 2 || json[start] != '{' || json[end - 1] != '}') return false;
    for (size_t index = start; index < end; index++) {
        unsigned char character = (unsigned char)json[index];
        if (character == '\0' || (character < 0x20 && !isspace(character))) return false;
    }
    if (start > 0) memmove(json, json + start, end - start);
    json[end - start] = '\0';
    return true;
}

static bool parse_port_span(const char *value, size_t length, unsigned int *port) {
    unsigned int number = 0;
    if (length == 0 || length > 5 || (length > 1 && value[0] == '0')) return false;
    for (size_t index = 0; index < length; index++) {
        if (!isdigit((unsigned char)value[index])) return false;
        number = number * 10 + (unsigned int)(value[index] - '0');
    }
    if (number < 1024 || number > 65535) return false;
    *port = number;
    return true;
}

static bool parse_port_value(const char *value, unsigned int *port) {
    return value != NULL && parse_port_span(value, strlen(value), port);
}

static bool valid_srt_hostname(const char *host, size_t length) {
    bool numeric = true;
    size_t label_length = 0;
    if (length == 0 || length > 253 || !isalnum((unsigned char)host[0]) ||
            !isalnum((unsigned char)host[length - 1])) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)host[index];
        if (character == '.') {
            if (label_length == 0 || label_length > 63 || host[index - 1] == '-') return false;
            label_length = 0;
            continue;
        }
        if (!isdigit(character)) numeric = false;
        if (!isalnum(character) && character != '-' && character != '_') return false;
        if (label_length == 0 && character == '-') return false;
        label_length++;
    }
    if (label_length == 0 || label_length > 63 || host[length - 1] == '-') return false;
    if (numeric) {
        struct in_addr address;
        char ipv4[INET_ADDRSTRLEN];
        if (length >= sizeof(ipv4)) return false;
        memcpy(ipv4, host, length);
        ipv4[length] = '\0';
        return inet_pton(AF_INET, ipv4, &address) == 1;
    }
    return true;
}

static bool parse_srt_url(const char *value, struct srt_endpoint *endpoint) {
    const char *authority;
    const char *authority_end;
    const char *port_start;
    const char *query;
    bool mode_seen = false;
    bool local_port = false;
    size_t length;

    if (value == NULL || strncmp(value, "srt://", 6) != 0 || contains_line_break(value)) return false;
    length = strlen(value);
    if (length > 2048 || strchr(value + 6, '#') != NULL) return false;
    authority = value + 6;
    query = strchr(authority, '?');
    if (query != NULL && strchr(query + 1, '?') != NULL) return false;
    authority_end = query == NULL ? value + length : query;
    if (authority == authority_end) return false;

    if (*authority == '[') {
        struct in6_addr address;
        char host[INET6_ADDRSTRLEN];
        const char *closing = memchr(authority, ']', (size_t)(authority_end - authority));
        size_t host_length;
        if (closing == NULL || closing == authority + 1 || closing + 1 >= authority_end ||
                closing[1] != ':') return false;
        host_length = (size_t)(closing - authority - 1);
        if (host_length >= sizeof(host)) return false;
        memcpy(host, authority + 1, host_length);
        host[host_length] = '\0';
        if (inet_pton(AF_INET6, host, &address) != 1) return false;
        port_start = closing + 2;
    } else {
        const char *separator = memchr(authority, ':', (size_t)(authority_end - authority));
        if (separator == NULL || memchr(separator + 1, ':', (size_t)(authority_end - separator - 1)) != NULL ||
                !valid_srt_hostname(authority, (size_t)(separator - authority))) return false;
        port_start = separator + 1;
    }
    if (!parse_port_span(port_start, (size_t)(authority_end - port_start), &endpoint->port)) return false;

    if (query != NULL) {
        const char *cursor = query + 1;
        while (true) {
            const char *end = strchr(cursor, '&');
            const char *separator;
            size_t key_length;
            if (end == NULL) end = value + length;
            if (end == cursor) return false;
            separator = memchr(cursor, '=', (size_t)(end - cursor));
            key_length = separator == NULL ? (size_t)(end - cursor) : (size_t)(separator - cursor);
            if (key_length == 4 && memcmp(cursor, "mode", 4) == 0) {
                size_t mode_length;
                const char *mode;
                if (mode_seen || separator == NULL) return false;
                mode_seen = true;
                mode = separator + 1;
                mode_length = (size_t)(end - mode);
                if (mode_length == 6 && memcmp(mode, "caller", 6) == 0) {
                    local_port = false;
                } else if ((mode_length == 8 && memcmp(mode, "listener", 8) == 0) ||
                        (mode_length == 10 && memcmp(mode, "rendezvous", 10) == 0)) {
                    local_port = true;
                } else {
                    return false;
                }
            }
            if (*end == '\0') break;
            cursor = end + 1;
        }
    }
    endpoint->local_port = local_port;
    return true;
}

static bool reserve_udp_port(struct udp_reservation *reservations, size_t *count,
        unsigned int port, unsigned int channel_id, const char *field,
        char *error, size_t error_capacity) {
    if (port >= 20000 && port <= 20100) {
        (void)snprintf(error, error_capacity,
                "{\"error\":\"UDP port %u for Channel %u %s conflicts with fixed ICE range 20000-20100\"}",
                port, channel_id, field);
        return false;
    }
    for (size_t index = 0; index < *count; index++) {
        if (reservations[index].port == port) {
            (void)snprintf(error, error_capacity,
                    "{\"error\":\"UDP port %u conflicts between Channel %u %s and Channel %u %s\"}",
                    port, reservations[index].channel_id, reservations[index].field, channel_id, field);
            return false;
        }
    }
    reservations[*count] = (struct udp_reservation){port, channel_id, field};
    (*count)++;
    return true;
}

static bool validate_udp_reservations(unsigned int candidate_channel,
        const char *const candidate_ports[4], const char *candidate_srt_url, bool candidate_uses_srt,
        char *error, size_t error_capacity) {
    static const char *const port_fields[4] = {
        "VIDEO_PORT", "AUDIO_PORT", "VIDEO_RTCP_PORT", "AUDIO_RTCP_PORT"
    };
    struct udp_reservation reservations[CHANNEL_COUNT * 5];
    size_t reservation_count = 0;

    for (unsigned int channel_id = 1; channel_id <= CHANNEL_COUNT; channel_id++) {
        struct channel_config config;
        struct srt_endpoint srt_endpoint;
        const char *ports[4];
        const char *srt_url;

        load_channel_config(channel_id, &config);
        if (channel_id == candidate_channel) {
            for (size_t index = 0; index < 4; index++) ports[index] = candidate_ports[index];
            srt_url = candidate_srt_url;
        } else {
            ports[0] = config.video_port;
            ports[1] = config.audio_port;
            ports[2] = config.video_rtcp_port;
            ports[3] = config.audio_rtcp_port;
            srt_url = config.srt_url;
        }
        for (size_t index = 0; index < 4; index++) {
            unsigned int port;
            if (!parse_port_value(ports[index], &port)) {
                (void)snprintf(error, error_capacity,
                        "{\"error\":\"Channel %u %s must be an integer between 1024 and 65535\"}",
                        channel_id, port_fields[index]);
                return false;
            }
            if (!reserve_udp_port(reservations, &reservation_count, port, channel_id,
                    port_fields[index], error, error_capacity)) return false;
        }
        if (!parse_srt_url(srt_url, &srt_endpoint)) {
            if (channel_id == candidate_channel && !candidate_uses_srt) continue;
            if (channel_id != candidate_channel && strcmp(config.channel_enabled, "false") == 0) continue;
            if (channel_id != candidate_channel && strcmp(config.input_mode, "rtp") == 0) continue;
            (void)snprintf(error, error_capacity,
                    "{\"error\":\"Channel %u SRT_URL is invalid\"}", channel_id);
            return false;
        }
        if (srt_endpoint.local_port && !reserve_udp_port(reservations, &reservation_count,
                srt_endpoint.port, channel_id, "SRT_URL local port", error, error_capacity)) return false;
    }
    return true;
}

static bool bridge_network_mode(void) {
    const char *mode = getenv("NETWORK_MODE");
    if (mode == NULL || *mode == '\0') mode = "bridge";
    return strcmp(mode, "bridge") == 0;
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
            !append_json_member(response, capacity, length, "VIDEO_PORT", config->video_port, true) ||
            !append_json_member(response, capacity, length, "AUDIO_PORT", config->audio_port, true) ||
            !append_json_member(response, capacity, length, "VIDEO_RTCP_PORT", config->video_rtcp_port, true) ||
            !append_json_member(response, capacity, length, "AUDIO_RTCP_PORT", config->audio_rtcp_port, true) ||
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
            "},\"fixed\":{\"STREAM_ID\":%u,\"HTTP_PORT\":8088,"
            "\"ICE_PORTS\":\"20000-20100/udp\"}}", channel_id);
    return fixed_length >= 0 && (size_t)fixed_length < sizeof(fixed) &&
            append_literal(response, capacity, length, fixed);
}

static void handle_get(int client) {
    char response[RESPONSE_CAPACITY];
    char network_json[NETWORK_JSON_CAPACITY];
    struct global_config global;
    struct channel_config config;
    size_t length = 0;
    bool ok;

    response[0] = '\0';
    load_global_config(&global);
    if (!load_network_json(network_json, sizeof(network_json))) {
        send_response(client, 500, "{\"error\":\"Effective network state is unavailable\"}");
        return;
    }
    ok = append_literal(response, sizeof(response), &length,
            "{\"version\":3,\"global\":{\"PUBLIC_IP\":") &&
            append_json_string(response, sizeof(response), &length, global.public_ip) &&
            append_literal(response, sizeof(response), &length, ",\"MANAGEMENT_INTERFACE\":") &&
            append_json_string(response, sizeof(response), &length, global.management_interface) &&
            append_literal(response, sizeof(response), &length, ",\"INGEST_INTERFACE\":") &&
            append_json_string(response, sizeof(response), &length, global.ingest_interface) &&
            append_literal(response, sizeof(response), &length, ",\"WEBRTC_INTERFACE\":") &&
            append_json_string(response, sizeof(response), &length, global.webrtc_interface) &&
            append_literal(response, sizeof(response), &length, "},\"network\":") &&
            append_literal(response, sizeof(response), &length, network_json) &&
            append_literal(response, sizeof(response), &length, ",\"channels\":[");
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
    const char *management_interface = field_value(fields, count, "MANAGEMENT_INTERFACE");
    const char *ingest_interface = field_value(fields, count, "INGEST_INTERFACE");
    const char *webrtc_interface = field_value(fields, count, "WEBRTC_INTERFACE");
    struct config_pair global_pairs[4];
    struct file_snapshot global_snapshot;
    struct global_config old_config;

    if (!valid_public_ip(public_ip) || !valid_interface_selector(management_interface) ||
            !valid_interface_selector(ingest_interface) || !valid_interface_selector(webrtc_interface)) {
        send_response(client, 400, "{\"error\":\"Global settings are invalid\"}");
        return;
    }
    load_global_config(&old_config);
    if (strcmp(old_config.public_ip, public_ip) == 0 &&
            strcmp(old_config.management_interface, management_interface) == 0 &&
            strcmp(old_config.ingest_interface, ingest_interface) == 0 &&
            strcmp(old_config.webrtc_interface, webrtc_interface) == 0) {
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
    global_pairs[1] = (struct config_pair){"MANAGEMENT_INTERFACE", management_interface};
    global_pairs[2] = (struct config_pair){"INGEST_INTERFACE", ingest_interface};
    global_pairs[3] = (struct config_pair){"WEBRTC_INTERFACE", webrtc_interface};
    if (!atomic_write_pairs(GLOBAL_CONFIG, global_pairs, 4)) {
        bool restored = restore_snapshot(GLOBAL_CONFIG, &global_snapshot);
        free_snapshot(&global_snapshot);
        send_response(client, 500, restored ?
                "{\"error\":\"Could not persist global settings\"}" :
                "{\"error\":\"Could not persist or restore global settings\"}");
        return;
    }
    if (!request_apply(0, true)) {
        bool restored = restore_snapshot(GLOBAL_CONFIG, &global_snapshot);
        bool reapplied = restored && request_apply(0, true);
        free_snapshot(&global_snapshot);
        send_response(client, 500, reapplied ?
                "{\"error\":\"Services could not apply global settings; previous settings were restored\"}" :
                "{\"error\":\"Services could not apply global settings and runtime rollback was incomplete\"}");
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
    const char *video_port = field_value(fields, count, "VIDEO_PORT");
    const char *audio_port = field_value(fields, count, "AUDIO_PORT");
    const char *video_rtcp_port = field_value(fields, count, "VIDEO_RTCP_PORT");
    const char *audio_rtcp_port = field_value(fields, count, "AUDIO_RTCP_PORT");
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
    const char *candidate_ports[4] = {video_port, audio_port, video_rtcp_port, audio_rtcp_port};
    unsigned int channel_id;
    unsigned int candidate_port_numbers[4];
    unsigned int old_port_numbers[4];
    struct srt_endpoint proposed_srt;
    struct srt_endpoint old_srt;
    struct channel_config old_config;
    struct config_pair channel_pairs[21];
    struct file_snapshot channel_snapshot;
    const char *selected_passphrase;
    char channel_path[512];
    char validation_error[256];
    char success_response[128];
    bool channel_uses_srt;
    bool proposed_srt_valid;

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
    channel_uses_srt = channel_enabled != NULL && mode != NULL &&
            strcmp(channel_enabled, "false") != 0 && strcmp(mode, "rtp") != 0;
    proposed_srt_valid = parse_srt_url(srt_url, &proposed_srt);
    if (!integer_in_range(channel_id_value, 1, CHANNEL_COUNT) || !valid_channel_name(channel_name) ||
            !one_of(channel_enabled, booleans) || !one_of(mode, modes) ||
            (!proposed_srt_valid && channel_uses_srt) ||
            !one_of(srt_audio, booleans) || !one_of(audio_enabled, booleans) ||
            !parse_port_value(video_port, &candidate_port_numbers[0]) ||
            !parse_port_value(audio_port, &candidate_port_numbers[1]) ||
            !parse_port_value(video_rtcp_port, &candidate_port_numbers[2]) ||
            !parse_port_value(audio_rtcp_port, &candidate_port_numbers[3]) ||
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
    if (bridge_network_mode() &&
            (!parse_port_value(old_config.video_port, &old_port_numbers[0]) ||
             !parse_port_value(old_config.audio_port, &old_port_numbers[1]) ||
             !parse_port_value(old_config.video_rtcp_port, &old_port_numbers[2]) ||
             !parse_port_value(old_config.audio_rtcp_port, &old_port_numbers[3]) ||
             candidate_port_numbers[0] != old_port_numbers[0] ||
             candidate_port_numbers[1] != old_port_numbers[1] ||
             candidate_port_numbers[2] != old_port_numbers[2] ||
             candidate_port_numbers[3] != old_port_numbers[3])) {
        send_response(client, 400,
                "{\"error\":\"Direct RTP/RTCP ports cannot be changed while NETWORK_MODE=bridge\"}");
        return;
    }
    if (bridge_network_mode() && proposed_srt_valid && proposed_srt.local_port &&
            (!parse_srt_url(old_config.srt_url, &old_srt) || !old_srt.local_port ||
             proposed_srt.port != old_srt.port)) {
        send_response(client, 400,
                "{\"error\":\"A local SRT listener or rendezvous port cannot be added or changed while NETWORK_MODE=bridge\"}");
        return;
    }
    if (!validate_udp_reservations(channel_id, candidate_ports, srt_url, channel_uses_srt,
            validation_error, sizeof(validation_error))) {
        send_response(client, 400, validation_error);
        return;
    }
    selected_passphrase = strcmp(passphrase_action, "set") == 0 ? passphrase :
            strcmp(passphrase_action, "clear") == 0 ? "" : old_config.srt_passphrase;

    channel_pairs[0] = (struct config_pair){"CHANNEL_NAME", channel_name};
    channel_pairs[1] = (struct config_pair){"CHANNEL_ENABLED", channel_enabled};
    channel_pairs[2] = (struct config_pair){"INPUT_MODE", mode};
    channel_pairs[3] = (struct config_pair){"SRT_URL", srt_url};
    channel_pairs[4] = (struct config_pair){"SRT_AUDIO", srt_audio};
    channel_pairs[5] = (struct config_pair){"AUDIO_ENABLED", audio_enabled};
    channel_pairs[6] = (struct config_pair){"VIDEO_PORT", video_port};
    channel_pairs[7] = (struct config_pair){"AUDIO_PORT", audio_port};
    channel_pairs[8] = (struct config_pair){"VIDEO_RTCP_PORT", video_rtcp_port};
    channel_pairs[9] = (struct config_pair){"AUDIO_RTCP_PORT", audio_rtcp_port};
    channel_pairs[10] = (struct config_pair){"SRT_COLOR_MODE", color_mode};
    channel_pairs[11] = (struct config_pair){"VIDEO_PRESET", video_preset};
    channel_pairs[12] = (struct config_pair){"VIDEO_BITRATE", video_bitrate};
    channel_pairs[13] = (struct config_pair){"VIDEO_BUFFER_SIZE", video_buffer_size};
    channel_pairs[14] = (struct config_pair){"AUDIO_BITRATE", audio_bitrate};
    channel_pairs[15] = (struct config_pair){"MAX_WIDTH", max_width};
    channel_pairs[16] = (struct config_pair){"MAX_HEIGHT", max_height};
    channel_pairs[17] = (struct config_pair){"MAX_FPS", max_fps};
    channel_pairs[18] = (struct config_pair){"INPUT_TIMEOUT_MS", timeout};
    channel_pairs[19] = (struct config_pair){"SRT_PBKEYLEN", pbkeylen};
    channel_pairs[20] = (struct config_pair){"SRT_PASSPHRASE", selected_passphrase};

    if (!ensure_config_directories()) {
        send_response(client, 500, "{\"error\":\"Could not prepare configuration storage\"}");
        return;
    }
    snprintf(channel_path, sizeof(channel_path), "%s/channel-%u.conf", CHANNELS_DIR, channel_id);
    if (!snapshot_file(channel_path, &channel_snapshot)) {
        send_response(client, 500, "{\"error\":\"Could not read existing channel settings\"}");
        return;
    }
    if (!atomic_write_pairs(channel_path, channel_pairs,
            sizeof(channel_pairs) / sizeof(channel_pairs[0]))) {
        bool restored = restore_snapshot(channel_path, &channel_snapshot);
        free_snapshot(&channel_snapshot);
        send_response(client, 500, restored ?
                "{\"error\":\"Could not persist settings\"}" :
                "{\"error\":\"Could not persist or restore settings\"}");
        return;
    }

    if (!request_apply(channel_id, false)) {
        bool channel_restored = restore_snapshot(channel_path, &channel_snapshot);
        bool runtime_restored = channel_restored && request_apply(channel_id, false);
        free_snapshot(&channel_snapshot);
        send_response(client, 500, runtime_restored ?
                "{\"error\":\"Services could not apply settings; previous settings were restored and reapplied\"}" :
                "{\"error\":\"Services could not apply settings and runtime rollback was incomplete\"}");
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

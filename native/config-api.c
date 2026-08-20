#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
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
#define FIELD_CAPACITY 32
#define SETTINGS_DIR "/config/settings"
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
    char *body;
    size_t content_length;
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void trim_line(char *value) {
    value[strcspn(value, "\r\n")] = '\0';
}

static bool read_file_line(const char *path, char *value, size_t capacity) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    if (fgets(value, (int)capacity, file) == NULL) {
        value[0] = '\0';
    }
    fclose(file);
    trim_line(value);
    return true;
}

static void setting_value(const char *name, const char *fallback, char *value, size_t capacity) {
    char path[512];
    const char *environment;

    snprintf(path, sizeof(path), "%s/%s", SETTINGS_DIR, name);
    if (read_file_line(path, value, capacity)) {
        return;
    }
    environment = getenv(name);
    snprintf(value, capacity, "%s", environment != NULL ? environment : fallback);
}

static bool write_setting(const char *name, const char *value) {
    char path[512];
    char temporary[512];
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", SETTINGS_DIR, name);
    snprintf(temporary, sizeof(temporary), "%s/.%s.tmp.%ld", SETTINGS_DIR, name, (long)getpid());
    file = fopen(temporary, "w");
    if (file == NULL) {
        return false;
    }
    bool ok = fprintf(file, "%s\n", value) >= 0;
    if (fclose(file) != 0) ok = false;
    if (!ok || chmod(temporary, 0600) != 0 || rename(temporary, path) != 0) {
        (void)unlink(temporary);
        return false;
    }
    return true;
}

static bool append_json_string(char *output, size_t capacity, size_t *length, const char *value) {
    if (*length + 2 >= capacity) {
        return false;
    }
    output[(*length)++] = '"';
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        const char *escape = NULL;
        char unicode_escape[7];
        if (*cursor == '"') escape = "\\\"";
        else if (*cursor == '\\') escape = "\\\\";
        else if (*cursor == '\n') escape = "\\n";
        else if (*cursor == '\r') escape = "\\r";
        else if (*cursor == '\t') escape = "\\t";
        else if (*cursor < 0x20) {
            snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *cursor);
            escape = unicode_escape;
        }
        if (escape != NULL) {
            size_t escape_length = strlen(escape);
            if (*length + escape_length + 1 >= capacity) return false;
            memcpy(output + *length, escape, escape_length);
            *length += escape_length;
        } else {
            if (*length + 2 >= capacity) return false;
            output[(*length)++] = (char)*cursor;
        }
    }
    output[(*length)++] = '"';
    output[*length] = '\0';
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
    (void)send(client, header, (size_t)header_length, MSG_NOSIGNAL);
    (void)send(client, body, strlen(body), MSG_NOSIGNAL);
}

static bool header_value(const char *line, const char *name, char *output, size_t capacity) {
    size_t name_length = strlen(name);
    const char *value;
    if (strncasecmp(line, name, name_length) != 0 || line[name_length] != ':') {
        return false;
    }
    value = line + name_length + 1;
    while (*value == ' ' || *value == '\t') value++;
    snprintf(output, capacity, "%s", value);
    trim_line(output);
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
        if (header_value(line, "X-Requested-With", request->requested_with, sizeof(request->requested_with))) continue;
        (void)header_value(line, "Content-Length", content_length, sizeof(content_length));
    }
    request->content_length = (size_t)strtoul(content_length, NULL, 10);
    if (request->content_length >= REQUEST_CAPACITY - body_offset) return false;
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
        if (*source == '+') {
            *destination++ = ' ';
            source++;
        } else if (*source == '%') {
            int high;
            int low;
            if (source[1] == '\0' || source[2] == '\0' ||
                    (high = hex_value(source[1])) < 0 || (low = hex_value(source[2])) < 0) return false;
            *destination++ = (char)((high << 4) | low);
            source += 3;
        } else {
            *destination++ = *source++;
        }
    }
    *destination = '\0';
    return true;
}

static size_t parse_fields(char *body, struct field *fields) {
    size_t count = 0;
    char *pair;
    char *save = NULL;
    for (pair = strtok_r(body, "&", &save); pair != NULL && count < FIELD_CAPACITY;
            pair = strtok_r(NULL, "&", &save)) {
        char *separator = strchr(pair, '=');
        if (separator == NULL) continue;
        *separator = '\0';
        fields[count].name = pair;
        fields[count].value = separator + 1;
        if (!url_decode(fields[count].name) || !url_decode(fields[count].value)) return 0;
        count++;
    }
    return count;
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
    return *value == '\0' || inet_pton(AF_INET, value, address) == 1 || inet_pton(AF_INET6, value, address) == 1;
}

static bool valid_srt_url(const char *value) {
    return value != NULL && strncmp(value, "srt://", 6) == 0 && strlen(value) <= 2048 &&
            strchr(value, '\r') == NULL && strchr(value, '\n') == NULL;
}

static bool request_apply(void) {
    char generation[64];
    char temporary[512];
    char line[256];
    struct timespec now;
    FILE *request;

    clock_gettime(CLOCK_REALTIME, &now);
    snprintf(generation, sizeof(generation), "%lld-%ld", (long long)now.tv_sec, (long)getpid());
    snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", APPLY_REQUEST, (long)getpid());
    request = fopen(temporary, "w");
    if (request == NULL) return false;
    bool ok = fprintf(request, "%s\n", generation) >= 0;
    if (fclose(request) != 0) ok = false;
    if (!ok || rename(temporary, APPLY_REQUEST) != 0) {
        (void)unlink(temporary);
        return false;
    }

    for (unsigned int attempt = 0; attempt < 200; attempt++) {
        FILE *result = fopen(APPLY_RESULT, "r");
        char result_generation[128] = "";
        char result_status[32] = "";
        if (result != NULL) {
            while (fgets(line, sizeof(line), result) != NULL) {
                trim_line(line);
                if (strncmp(line, "generation=", 11) == 0) snprintf(result_generation, sizeof(result_generation), "%.*s", (int)sizeof(result_generation) - 1, line + 11);
                if (strncmp(line, "status=", 7) == 0) snprintf(result_status, sizeof(result_status), "%.*s", (int)sizeof(result_status) - 1, line + 7);
            }
            fclose(result);
            if (strcmp(result_generation, generation) == 0) return strcmp(result_status, "ok") == 0;
        }
        usleep(100000);
    }
    return false;
}

static void handle_get(int client) {
    char response[RESPONSE_CAPACITY];
    size_t length = 0;
    char input_mode[32], srt_url[4096], srt_audio[16], audio_enabled[16], color_mode[32];
    char video_preset[32], video_bitrate[32], audio_bitrate[32], max_width[16], max_height[16];
    char max_fps[16], timeout[16], stream_id[32], public_ip[128], pbkeylen[16], passphrase[128];
    char video_port[16], audio_port[16], video_rtcp[16], audio_rtcp[16], srt_port[16];

    setting_value("INPUT_MODE", "auto", input_mode, sizeof(input_mode));
    setting_value("SRT_URL", "srt://0.0.0.0:9000?mode=listener&latency=120000", srt_url, sizeof(srt_url));
    setting_value("SRT_AUDIO", "true", srt_audio, sizeof(srt_audio));
    setting_value("AUDIO_ENABLED", "true", audio_enabled, sizeof(audio_enabled));
    setting_value("SRT_COLOR_MODE", "auto", color_mode, sizeof(color_mode));
    setting_value("VIDEO_PRESET", "veryfast", video_preset, sizeof(video_preset));
    setting_value("VIDEO_BITRATE", "6M", video_bitrate, sizeof(video_bitrate));
    setting_value("AUDIO_BITRATE", "128k", audio_bitrate, sizeof(audio_bitrate));
    setting_value("MAX_WIDTH", "1920", max_width, sizeof(max_width));
    setting_value("MAX_HEIGHT", "1080", max_height, sizeof(max_height));
    setting_value("MAX_FPS", "60", max_fps, sizeof(max_fps));
    setting_value("INPUT_TIMEOUT_MS", "5000", timeout, sizeof(timeout));
    setting_value("STREAM_ID", "1", stream_id, sizeof(stream_id));
    setting_value("PUBLIC_IP", "", public_ip, sizeof(public_ip));
    setting_value("SRT_PBKEYLEN", "16", pbkeylen, sizeof(pbkeylen));
    setting_value("SRT_PASSPHRASE", "", passphrase, sizeof(passphrase));
    setting_value("VIDEO_PORT", "5004", video_port, sizeof(video_port));
    setting_value("AUDIO_PORT", "5005", audio_port, sizeof(audio_port));
    setting_value("VIDEO_RTCP_PORT", "5006", video_rtcp, sizeof(video_rtcp));
    setting_value("AUDIO_RTCP_PORT", "5007", audio_rtcp, sizeof(audio_rtcp));
    setting_value("SRT_PUBLIC_PORT", "9000", srt_port, sizeof(srt_port));

    length += (size_t)snprintf(response + length, sizeof(response) - length,
            "{\"passphraseConfigured\":%s,\"values\":{",
            *passphrase != '\0' ? "true" : "false");
#define JSON_SETTING(json_name, value) do { \
    length += (size_t)snprintf(response + length, sizeof(response) - length, "\"%s\":", json_name); \
    if (!append_json_string(response, sizeof(response), &length, value)) { send_response(client, 500, "{\"error\":\"Response too large\"}"); return; } \
    response[length++] = ','; response[length] = '\0'; \
} while (0)
    JSON_SETTING("INPUT_MODE", input_mode);
    JSON_SETTING("SRT_URL", srt_url);
    JSON_SETTING("SRT_AUDIO", srt_audio);
    JSON_SETTING("AUDIO_ENABLED", audio_enabled);
    JSON_SETTING("SRT_COLOR_MODE", color_mode);
    JSON_SETTING("VIDEO_PRESET", video_preset);
    JSON_SETTING("VIDEO_BITRATE", video_bitrate);
    JSON_SETTING("AUDIO_BITRATE", audio_bitrate);
    JSON_SETTING("MAX_WIDTH", max_width);
    JSON_SETTING("MAX_HEIGHT", max_height);
    JSON_SETTING("MAX_FPS", max_fps);
    JSON_SETTING("INPUT_TIMEOUT_MS", timeout);
    JSON_SETTING("STREAM_ID", stream_id);
    JSON_SETTING("PUBLIC_IP", public_ip);
    JSON_SETTING("SRT_PBKEYLEN", pbkeylen);
    length--;
    length += (size_t)snprintf(response + length, sizeof(response) - length,
            "},\"fixed\":{\"VIDEO_PORT\":%s,\"AUDIO_PORT\":%s,\"VIDEO_RTCP_PORT\":%s,\"AUDIO_RTCP_PORT\":%s,\"SRT_PORT\":%s,\"HTTP_PORT\":8088,\"ICE_PORTS\":\"20000-20100/udp\"}}",
            video_port, audio_port, video_rtcp, audio_rtcp, srt_port);
#undef JSON_SETTING
    send_response(client, 200, response);
}

static void handle_post(int client, struct field *fields, size_t count) {
    static const char *const modes[] = {"auto", "rtp", "srt", NULL};
    static const char *const booleans[] = {"true", "false", NULL};
    static const char *const colors[] = {"auto", "fast", "hdr-to-sdr", NULL};
    static const char *const presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", NULL};
    static const char *const passphrase_actions[] = {"keep", "set", "clear", NULL};
    const char *mode = field_value(fields, count, "INPUT_MODE");
    const char *srt_url = field_value(fields, count, "SRT_URL");
    const char *srt_audio = field_value(fields, count, "SRT_AUDIO");
    const char *audio_enabled = field_value(fields, count, "AUDIO_ENABLED");
    const char *color_mode = field_value(fields, count, "SRT_COLOR_MODE");
    const char *video_preset = field_value(fields, count, "VIDEO_PRESET");
    const char *video_bitrate = field_value(fields, count, "VIDEO_BITRATE");
    const char *audio_bitrate = field_value(fields, count, "AUDIO_BITRATE");
    const char *max_width = field_value(fields, count, "MAX_WIDTH");
    const char *max_height = field_value(fields, count, "MAX_HEIGHT");
    const char *max_fps = field_value(fields, count, "MAX_FPS");
    const char *timeout = field_value(fields, count, "INPUT_TIMEOUT_MS");
    const char *stream_id = field_value(fields, count, "STREAM_ID");
    const char *public_ip = field_value(fields, count, "PUBLIC_IP");
    const char *pbkeylen = field_value(fields, count, "SRT_PBKEYLEN");
    const char *passphrase_action = field_value(fields, count, "passphraseAction");
    const char *passphrase = field_value(fields, count, "SRT_PASSPHRASE");

    if (!one_of(mode, modes) || !valid_srt_url(srt_url) || !one_of(srt_audio, booleans) ||
            !one_of(audio_enabled, booleans) || !one_of(color_mode, colors) ||
            !one_of(video_preset, presets) || !valid_bitrate(video_bitrate) ||
            !valid_bitrate(audio_bitrate) || !integer_in_range(max_width, 16, 8192) ||
            !integer_in_range(max_height, 16, 8192) || !integer_in_range(max_fps, 1, 60) ||
            !integer_in_range(timeout, 1000, 60000) || !integer_in_range(stream_id, 1, 2147483647) ||
            !valid_public_ip(public_ip) || !(strcmp(pbkeylen, "16") == 0 || strcmp(pbkeylen, "24") == 0 || strcmp(pbkeylen, "32") == 0) ||
            !one_of(passphrase_action, passphrase_actions) ||
            (strcmp(passphrase_action, "set") == 0 && (passphrase == NULL || strlen(passphrase) < 10 || strlen(passphrase) > 79 || strchr(passphrase, '\n') != NULL))) {
        send_response(client, 400, "{\"error\":\"One or more settings are invalid\"}");
        return;
    }

#define SAVE_SETTING(name, value) do { if (!write_setting(name, value)) { send_response(client, 500, "{\"error\":\"Could not persist settings\"}"); return; } } while (0)
    SAVE_SETTING("INPUT_MODE", mode);
    SAVE_SETTING("SRT_URL", srt_url);
    SAVE_SETTING("SRT_AUDIO", srt_audio);
    SAVE_SETTING("AUDIO_ENABLED", audio_enabled);
    SAVE_SETTING("SRT_COLOR_MODE", color_mode);
    SAVE_SETTING("VIDEO_PRESET", video_preset);
    SAVE_SETTING("VIDEO_BITRATE", video_bitrate);
    SAVE_SETTING("AUDIO_BITRATE", audio_bitrate);
    SAVE_SETTING("MAX_WIDTH", max_width);
    SAVE_SETTING("MAX_HEIGHT", max_height);
    SAVE_SETTING("MAX_FPS", max_fps);
    SAVE_SETTING("INPUT_TIMEOUT_MS", timeout);
    SAVE_SETTING("STREAM_ID", stream_id);
    SAVE_SETTING("PUBLIC_IP", public_ip);
    SAVE_SETTING("SRT_PBKEYLEN", pbkeylen);
    if (strcmp(passphrase_action, "set") == 0) SAVE_SETTING("SRT_PASSPHRASE", passphrase);
    if (strcmp(passphrase_action, "clear") == 0) SAVE_SETTING("SRT_PASSPHRASE", "");
#undef SAVE_SETTING

    if (!request_apply()) {
        send_response(client, 500, "{\"error\":\"Settings were saved but services could not be restarted\"}");
        return;
    }
    send_response(client, 200, "{\"ok\":true,\"message\":\"Settings applied\"}");
}

static void handle_client(int client) {
    char buffer[REQUEST_CAPACITY];
    struct http_request request;
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
        send_response(client, 401, "{\"error\":\"Configuration requests must come from the application UI\"}");
        return;
    }
    struct field fields[FIELD_CAPACITY];
    size_t field_count = parse_fields(request.body, fields);
    if (field_count == 0) {
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
            bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 16) != 0) {
        fprintf(stderr, "[config-api] startup failed: %s\n", strerror(errno));
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

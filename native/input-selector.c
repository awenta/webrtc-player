#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHANNEL_COUNT 4
#define MAX_INPUTS (CHANNEL_COUNT * 2)
#define RTP_HEADER_SIZE 12
#define REQUIRED_VIDEO_PACKETS 2
#define OUTPUT_QUEUE_CAPACITY 16384
#define VIDEO_PACING_BATCH 8

enum source_id {
    SOURCE_NONE = 0,
    SOURCE_DIRECT = 1,
    SOURCE_SRT = 2,
};

enum channel_id {
    VIDEO_RTP = 0,
    AUDIO_RTP = 1,
    VIDEO_RTCP = 2,
    AUDIO_RTCP = 3,
};

struct input_socket {
    int fd;
    enum source_id source;
    enum channel_id channel;
};

struct candidate {
    uint32_t ssrc;
    uint16_t sequence;
    unsigned int consecutive;
    int64_t last_packet_ms;
};

struct queued_packet {
    uint8_t *data;
    size_t length;
    enum channel_id channel;
};

struct output_queue {
    struct queued_packet packets[OUTPUT_QUEUE_CAPACITY];
    struct sockaddr_in outputs[CHANNEL_COUNT];
    pthread_mutex_t mutex;
    pthread_cond_t available;
    size_t head;
    size_t count;
    uint64_t dropped;
    long video_pacing_ns;
    int fd;
    bool stopping;
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int read_number(const char *name, int fallback, int minimum, int maximum) {
    const char *raw = getenv(name);
    char *end = NULL;
    long value;

    if (raw == NULL || *raw == '\0') {
        return fallback;
    }
    errno = 0;
    value = strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < minimum || value > maximum) {
        fprintf(stderr, "[input-selector] invalid %s\n", name);
        exit(EXIT_FAILURE);
    }
    return (int)value;
}

static const char *source_name(enum source_id source) {
    switch (source) {
        case SOURCE_DIRECT:
            return "rtp";
        case SOURCE_SRT:
            return "srt";
        default:
            return "none";
    }
}

static int bind_input(int port, const struct in_addr *bind_address) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int receive_buffer = 4 * 1024 * 1024;
    int reuse = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr = *bind_address,
    };

    if (fd < 0) {
        perror("[input-selector] socket");
        exit(EXIT_FAILURE);
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        fprintf(stderr, "[input-selector] cannot bind UDP port %d: %s\n", port, strerror(errno));
        close(fd);
        exit(EXIT_FAILURE);
    }
    return fd;
}

static bool valid_rtp(const uint8_t *packet, size_t length, uint8_t payload_type) {
    return length >= RTP_HEADER_SIZE && (packet[0] >> 6) == 2 && (packet[1] & 0x7f) == payload_type;
}

static bool valid_rtcp(const uint8_t *packet, size_t length) {
    return length >= 8 && (packet[0] >> 6) == 2 && packet[1] >= 192 && packet[1] <= 223;
}

static bool candidate_ready(struct candidate *candidate, const uint8_t *packet, int64_t now_ms) {
    uint16_t sequence;
    uint32_t ssrc;
    uint16_t sequence_delta;

    memcpy(&sequence, packet + 2, sizeof(sequence));
    memcpy(&ssrc, packet + 8, sizeof(ssrc));
    sequence = ntohs(sequence);
    ssrc = ntohl(ssrc);
    sequence_delta = (uint16_t)(sequence - candidate->sequence);

    if (candidate->consecutive > 0 && candidate->ssrc == ssrc &&
            now_ms - candidate->last_packet_ms <= 1000 && sequence_delta > 0 && sequence_delta <= 100) {
        candidate->consecutive++;
    } else {
        candidate->ssrc = ssrc;
        candidate->consecutive = 1;
    }
    candidate->sequence = sequence;
    candidate->last_packet_ms = now_ms;
    return candidate->consecutive >= REQUIRED_VIDEO_PACKETS;
}

static void count_output_drop(struct output_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->dropped++;
    pthread_mutex_unlock(&queue->mutex);
}

static void *forward_packets(void *opaque) {
    struct output_queue *queue = opaque;
    unsigned int video_packets_since_pause = 0;

    while (true) {
        struct queued_packet packet;

        pthread_mutex_lock(&queue->mutex);
        while (queue->count == 0 && !queue->stopping) {
            pthread_cond_wait(&queue->available, &queue->mutex);
        }
        if (queue->count == 0 && queue->stopping) {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }
        packet = queue->packets[queue->head];
        queue->head = (queue->head + 1) % OUTPUT_QUEUE_CAPACITY;
        queue->count--;
        pthread_mutex_unlock(&queue->mutex);

        if (sendto(queue->fd, packet.data, packet.length, MSG_DONTWAIT,
                (const struct sockaddr *)&queue->outputs[packet.channel], sizeof(queue->outputs[0])) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS) {
                fprintf(stderr, "[input-selector] forward failed: %s\n", strerror(errno));
            }
            count_output_drop(queue);
        }
        free(packet.data);

        if (packet.channel == VIDEO_RTP && queue->video_pacing_ns > 0 &&
                ++video_packets_since_pause == VIDEO_PACING_BATCH) {
            long pause_ns = queue->video_pacing_ns * VIDEO_PACING_BATCH;
            const struct timespec delay = {
                .tv_sec = pause_ns / 1000000000L,
                .tv_nsec = pause_ns % 1000000000L,
            };
            (void)nanosleep(&delay, NULL);
            video_packets_since_pause = 0;
        }
    }
    return NULL;
}

static void enqueue_packet(struct output_queue *queue, enum channel_id channel,
        const uint8_t *data, size_t length) {
    uint8_t *copy = malloc(length);

    if (copy == NULL) {
        count_output_drop(queue);
        return;
    }
    memcpy(copy, data, length);

    pthread_mutex_lock(&queue->mutex);
    if (queue->count == OUTPUT_QUEUE_CAPACITY) {
        queue->dropped++;
        pthread_mutex_unlock(&queue->mutex);
        free(copy);
        return;
    }
    size_t tail = (queue->head + queue->count) % OUTPUT_QUEUE_CAPACITY;
    queue->packets[tail] = (struct queued_packet){
        .data = copy,
        .length = length,
        .channel = channel,
    };
    queue->count++;
    pthread_cond_signal(&queue->available);
    pthread_mutex_unlock(&queue->mutex);
}

static uint64_t output_drop_count(struct output_queue *queue) {
    uint64_t dropped;

    pthread_mutex_lock(&queue->mutex);
    dropped = queue->dropped;
    pthread_mutex_unlock(&queue->mutex);
    return dropped;
}

static void write_status(const char *status_path, enum source_id active_source, bool direct_enabled,
        bool srt_enabled, uint64_t direct_packets, uint64_t srt_packets, uint64_t switches,
        uint64_t forward_drops, const char *remote_address, unsigned int remote_port, int64_t updated_ms) {
    char temporary_path[512];
    FILE *status;

    if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.%ld", status_path, (long)getpid()) >=
            (int)sizeof(temporary_path)) {
        fprintf(stderr, "[input-selector] status path is too long\n");
        return;
    }
    status = fopen(temporary_path, "w");
    if (status == NULL) {
        fprintf(stderr, "[input-selector] cannot write status: %s\n", strerror(errno));
        return;
    }
    fprintf(status,
             "version=1\nsource=%s\ndirect_enabled=%s\nsrt_enabled=%s\n"
             "direct_packets=%llu\nsrt_packets=%llu\nswitches=%llu\n"
             "forward_drops=%llu\n"
             "remote_address=%s\nremote_port=%u\nupdated_ms=%lld\n",
            source_name(active_source), direct_enabled ? "true" : "false", srt_enabled ? "true" : "false",
             (unsigned long long)direct_packets, (unsigned long long)srt_packets,
             (unsigned long long)switches, (unsigned long long)forward_drops,
             remote_address, remote_port, (long long)updated_ms);
    if (fclose(status) != 0 || rename(temporary_path, status_path) != 0) {
        fprintf(stderr, "[input-selector] cannot publish status: %s\n", strerror(errno));
        (void)unlink(temporary_path);
    }
}

int main(void) {
    const char *mode = getenv("INPUT_MODE");
    const char *status_path = getenv("SELECTOR_STATUS_PATH");
    const char *passphrase = getenv("SRT_PASSPHRASE");
    const char *passphrase_file = getenv("SRT_PASSPHRASE_FILE");
    const char *rtp_bind_ip = getenv("RTP_BIND_IP");
    struct in_addr direct_bind_address;
    const struct in_addr loopback_address = {.s_addr = htonl(INADDR_LOOPBACK)};
    bool direct_enabled;
    bool srt_enabled;
    int direct_ports[CHANNEL_COUNT] = {
        read_number("VIDEO_PORT", 5004, 1024, 65535),
        read_number("AUDIO_PORT", 5005, 1024, 65535),
        read_number("VIDEO_RTCP_PORT", 5006, 1024, 65535),
        read_number("AUDIO_RTCP_PORT", 5007, 1024, 65535),
    };
    int srt_ports[CHANNEL_COUNT] = {
        read_number("SRT_RELAY_VIDEO_PORT", 15004, 1024, 65535),
        read_number("SRT_RELAY_AUDIO_PORT", 15005, 1024, 65535),
        read_number("SRT_RELAY_VIDEO_RTCP_PORT", 15006, 1024, 65535),
        read_number("SRT_RELAY_AUDIO_RTCP_PORT", 15007, 1024, 65535),
    };
    int janus_ports[CHANNEL_COUNT] = {
        read_number("JANUS_VIDEO_PORT", 25004, 1024, 65535),
        read_number("JANUS_AUDIO_PORT", 25005, 1024, 65535),
        read_number("JANUS_VIDEO_RTCP_PORT", 25006, 1024, 65535),
        read_number("JANUS_AUDIO_RTCP_PORT", 25007, 1024, 65535),
    };
    int timeout_ms = read_number("INPUT_TIMEOUT_MS", 5000, 1000, 60000);
    int video_pacing_us = read_number("RTP_PACING_US", 100, 0, 2000);
    struct input_socket inputs[MAX_INPUTS];
    struct pollfd poll_descriptors[MAX_INPUTS];
    struct output_queue output_queue = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .available = PTHREAD_COND_INITIALIZER,
    };
    pthread_t output_thread;
    struct candidate candidates[3] = {0};
    size_t input_count = 0;
    int output_fd;
    enum source_id active_source = SOURCE_NONE;
    int64_t active_last_video_ms = 0;
    int64_t last_status_ms = 0;
    uint64_t packet_counts[3] = {0};
    uint64_t switches = 0;
    char remote_address[INET_ADDRSTRLEN] = "";
    unsigned int remote_port = 0;
    uint8_t packet[65536];

    if (status_path == NULL || *status_path == '\0') {
        status_path = "/run/webrtc-player/input-selector.status";
    }
    if (mode == NULL || *mode == '\0') {
        mode = "auto";
    }
    if (rtp_bind_ip == NULL || inet_pton(AF_INET, rtp_bind_ip, &direct_bind_address) != 1 ||
            direct_bind_address.s_addr == htonl(INADDR_ANY) ||
            direct_bind_address.s_addr == htonl(INADDR_LOOPBACK)) {
        fprintf(stderr, "[input-selector] RTP_BIND_IP must be a non-loopback IPv4 address\n");
        return EXIT_FAILURE;
    }
    direct_enabled = strcmp(mode, "auto") == 0 || strcmp(mode, "rtp") == 0;
    srt_enabled = strcmp(mode, "auto") == 0 || strcmp(mode, "srt") == 0;
    if (!direct_enabled && !srt_enabled) {
        fprintf(stderr, "[input-selector] INPUT_MODE must be auto, rtp, or srt\n");
        return EXIT_FAILURE;
    }
    if (strcmp(mode, "auto") == 0 && ((passphrase != NULL && *passphrase != '\0') ||
            (passphrase_file != NULL && *passphrase_file != '\0'))) {
        direct_enabled = false;
        printf("[input-selector] encrypted SRT configured; unauthenticated direct RTP is disabled\n");
    }

    for (size_t channel = 0; channel < CHANNEL_COUNT; channel++) {
        output_queue.outputs[channel].sin_family = AF_INET;
        output_queue.outputs[channel].sin_port = htons((uint16_t)janus_ports[channel]);
        output_queue.outputs[channel].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (direct_enabled) {
            inputs[input_count] = (struct input_socket){
                .fd = bind_input(direct_ports[channel], &direct_bind_address),
                .source = SOURCE_DIRECT,
                .channel = (enum channel_id)channel,
            };
            input_count++;
        }
        if (srt_enabled) {
            inputs[input_count] = (struct input_socket){
                .fd = bind_input(srt_ports[channel], &loopback_address),
                .source = SOURCE_SRT,
                .channel = (enum channel_id)channel,
            };
            input_count++;
        }
    }

    output_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (output_fd < 0) {
        perror("[input-selector] output socket");
        return EXIT_FAILURE;
    }
    output_queue.fd = output_fd;
    output_queue.video_pacing_ns = (long)video_pacing_us * 1000;
    if (pthread_create(&output_thread, NULL, forward_packets, &output_queue) != 0) {
        fprintf(stderr, "[input-selector] cannot start forwarding thread\n");
        close(output_fd);
        return EXIT_FAILURE;
    }
    for (size_t index = 0; index < input_count; index++) {
        poll_descriptors[index].fd = inputs[index].fd;
        poll_descriptors[index].events = POLLIN;
    }

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[input-selector] ready: direct-rtp=%s bind=%s srt=%s timeout=%dms pacing=%dus/%d packets\n",
            direct_enabled ? "enabled" : "disabled", rtp_bind_ip,
            srt_enabled ? "enabled" : "disabled",
            timeout_ms, video_pacing_us, VIDEO_PACING_BATCH);

    while (running) {
        int ready = poll(poll_descriptors, input_count, 250);
        int64_t now_ms = monotonic_ms();

        if (ready < 0 && errno != EINTR) {
            fprintf(stderr, "[input-selector] poll failed: %s\n", strerror(errno));
            break;
        }
        if (active_source != SOURCE_NONE && now_ms - active_last_video_ms > timeout_ms) {
            printf("[input-selector] %s input timed out; waiting for a source\n", source_name(active_source));
            active_source = SOURCE_NONE;
            active_last_video_ms = 0;
            remote_address[0] = '\0';
            remote_port = 0;
            memset(candidates, 0, sizeof(candidates));
        }

        for (size_t index = 0; ready > 0 && index < input_count; index++) {
            if ((poll_descriptors[index].revents & POLLIN) == 0) {
                continue;
            }
            ready--;
            while (running) {
                struct sockaddr_in sender;
                socklen_t sender_length = sizeof(sender);
                ssize_t length = recvfrom(inputs[index].fd, packet, sizeof(packet), 0,
                        (struct sockaddr *)&sender, &sender_length);
                bool valid;

                if (length < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        fprintf(stderr, "[input-selector] receive failed: %s\n", strerror(errno));
                    }
                    break;
                }
                valid = inputs[index].channel == VIDEO_RTP ? valid_rtp(packet, (size_t)length, 96) :
                        inputs[index].channel == AUDIO_RTP ? valid_rtp(packet, (size_t)length, 111) :
                        valid_rtcp(packet, (size_t)length);
                if (!valid) {
                    continue;
                }
                packet_counts[inputs[index].source]++;

                if (inputs[index].channel == VIDEO_RTP) {
                    if (active_source == SOURCE_NONE &&
                            candidate_ready(&candidates[inputs[index].source], packet, now_ms)) {
                        active_source = inputs[index].source;
                        active_last_video_ms = now_ms;
                        switches++;
                        if (active_source == SOURCE_DIRECT) {
                            (void)inet_ntop(AF_INET, &sender.sin_addr, remote_address, sizeof(remote_address));
                            remote_port = ntohs(sender.sin_port);
                        }
                        printf("[input-selector] selected %s input\n", source_name(active_source));
                    } else if (active_source == inputs[index].source) {
                        active_last_video_ms = now_ms;
                        if (active_source == SOURCE_DIRECT) {
                            (void)inet_ntop(AF_INET, &sender.sin_addr, remote_address, sizeof(remote_address));
                            remote_port = ntohs(sender.sin_port);
                        }
                    }
                }
                if (active_source != inputs[index].source) {
                    continue;
                }
                enqueue_packet(&output_queue, inputs[index].channel, packet, (size_t)length);
            }
        }

        if (now_ms - last_status_ms >= 1000) {
            write_status(status_path, active_source, direct_enabled, srt_enabled,
                    packet_counts[SOURCE_DIRECT], packet_counts[SOURCE_SRT], switches,
                    output_drop_count(&output_queue), remote_address, remote_port, now_ms);
            last_status_ms = now_ms;
        }
    }

    for (size_t index = 0; index < input_count; index++) {
        close(inputs[index].fd);
    }
    pthread_mutex_lock(&output_queue.mutex);
    output_queue.stopping = true;
    pthread_cond_signal(&output_queue.available);
    pthread_mutex_unlock(&output_queue.mutex);
    (void)pthread_join(output_thread, NULL);
    pthread_cond_destroy(&output_queue.available);
    pthread_mutex_destroy(&output_queue.mutex);
    close(output_fd);
    (void)unlink(status_path);
    return EXIT_SUCCESS;
}

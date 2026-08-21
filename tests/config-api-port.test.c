#define main config_api_program_main
#include "../native/config-api.c"
#undef main

static int failures;

static void expect_srt(const char *url, bool valid, bool local, unsigned int port) {
    struct srt_endpoint endpoint = {0};
    bool result = parse_srt_url(url, &endpoint);

    if (result != valid || (valid && (endpoint.local_port != local || endpoint.port != port))) {
        fprintf(stderr, "unexpected SRT parse result for %s\n", url);
        failures++;
    }
}

static void test_srt_urls(void) {
    expect_srt("srt://source.example:9000", true, false, 9000);
    expect_srt("srt://ingest.example:9001?mode=listener&latency=120000", true, true, 9001);
    expect_srt("srt://source.example:9002?latency=120000&mode=caller", true, false, 9002);
    expect_srt("srt://[2001:db8::1]:9003?mode=rendezvous", true, true, 9003);
    expect_srt("http://127.0.0.1:9000", false, false, 0);
    expect_srt("srt://127.0.0.1:1023", false, false, 0);
    expect_srt("srt://127.0.0.1:09000", false, false, 0);
    expect_srt("srt://127.0.0.1:65536", false, false, 0);
    expect_srt("srt://127.0.0.1:9000/path", false, false, 0);
    expect_srt("srt://127.0.0.1:9000#fragment", false, false, 0);
    expect_srt("srt://127.0.0.1:9000?", false, false, 0);
    expect_srt("srt://127.0.0.1:9000?mode=listener&&latency=1", false, false, 0);
    expect_srt("srt://127.0.0.1:9000?mode=listener&mode=caller", false, false, 0);
    expect_srt("srt://127.0.0.1:9000?mode=unsupported", false, false, 0);
    expect_srt("srt://127.0.0.1:9000?mode=listener?latency=1", false, false, 0);
}

static void test_legacy_listener_normalization(void) {
    char url[2049] = "srt://:9000?mode=listener&latency=120000";
    struct srt_endpoint endpoint = {0};

    normalize_legacy_srt_listener(url, sizeof(url));
    if (strcmp(url, "srt://0.0.0.0:9000?mode=listener&latency=120000") != 0 ||
            !parse_srt_url(url, &endpoint) || !endpoint.local_port || endpoint.port != 9000) {
        fprintf(stderr, "legacy empty-host SRT listener was not normalized\n");
        failures++;
    }
}

static void test_udp_reservations(void) {
    struct udp_reservation reservations[2];
    size_t count = 0;
    char error[256];

    if (!reserve_udp_port(reservations, &count, 5004, 1, "VIDEO_PORT", error, sizeof(error))) {
        fprintf(stderr, "first UDP reservation failed\n");
        failures++;
    }
    if (reserve_udp_port(reservations, &count, 5004, 2, "VIDEO_PORT", error, sizeof(error))) {
        fprintf(stderr, "duplicate UDP reservation was accepted\n");
        failures++;
    }
    count = 0;
    if (reserve_udp_port(reservations, &count, 20050, 1, "VIDEO_PORT", error, sizeof(error))) {
        fprintf(stderr, "ICE-range UDP reservation was accepted\n");
        failures++;
    }
}

int main(void) {
    test_srt_urls();
    test_legacy_listener_normalization();
    test_udp_reservations();
    if (failures != 0) return EXIT_FAILURE;
    puts("config API port tests passed");
    return EXIT_SUCCESS;
}

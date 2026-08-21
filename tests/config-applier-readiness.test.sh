#!/usr/bin/env bash
set -Eeuo pipefail

# shellcheck source=../scripts/config-applier.sh
source "$(dirname "$0")/../scripts/config-applier.sh"

fail() {
    echo "config applier readiness test failed: $*" >&2
    exit 1
}

load_network_settings() {
    INGEST_IP=192.0.2.10
}

load_channel_settings() {
    CHANNEL_ENABLED=true
    INPUT_MODE=auto
    AUDIO_ENABLED=true
    VIDEO_PORT=5004
    AUDIO_PORT=5005
    VIDEO_RTCP_PORT=5006
    AUDIO_RTCP_PORT=5007
    SRT_URL_VALID=true
    SRT_URL='srt://0.0.0.0:18000?mode=listener&latency=120000'
    SRT_PASSPHRASE=
    SRT_PASSPHRASE_FILE=
}

prepare_channel_readiness 1 || fail "listener readiness preparation failed: ${READY_REASON}"
[[ ${REQUIRE_FULL_READINESS[1]} == true ]] || fail "SRT listener did not require stable-process readiness"
[[ ${EXPECT_FFMPEG_PROCESS[1]} == true ]] || fail "SRT listener did not require FFmpeg"
[[ ${EXPECTED_IPV4_LISTENERS[1]} == *",5004"* ]] || fail "direct RTP listener was not verified"
[[ ${EXPECTED_IPV4_LISTENERS[1]} != *",18000"* ]] || fail "SRT listener still depended on /proc/net/udp"

echo "config applier readiness tests passed"

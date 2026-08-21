#!/usr/bin/env bash
set -Eeuo pipefail

unset CHANNEL_ID VIDEO_PORT AUDIO_PORT VIDEO_RTCP_PORT AUDIO_RTCP_PORT SRT_URL
# shellcheck source=../scripts/load-settings.sh
source "$(dirname "$0")/../scripts/load-settings.sh"

fail() {
    echo "settings port test failed: $*" >&2
    exit 1
}

expect_srt() {
    local url="$1" expected_port="$2" expected_mode="$3"
    _load_settings_parse_srt_url "${url}" || fail "expected valid SRT URL: ${url}"
    [[ ${REPLY} == "${expected_port}" && ${REPLY_MODE} == "${expected_mode}" ]] \
        || fail "unexpected parse result for ${url}"
}

reject_srt() {
    local url="$1"
    ! _load_settings_parse_srt_url "${url}" || fail "expected invalid SRT URL: ${url}"
}

[[ ${VIDEO_PORT} == 5004 && ${AUDIO_PORT} == 5005 && \
   ${VIDEO_RTCP_PORT} == 5006 && ${AUDIO_RTCP_PORT} == 5007 ]] \
    || fail "Channel 1 defaults changed"

load_channel_settings 5
[[ ${VIDEO_PORT} == 5020 && ${AUDIO_PORT} == 5021 && \
   ${VIDEO_RTCP_PORT} == 5022 && ${AUDIO_RTCP_PORT} == 5023 ]] \
    || fail "Channel 5 defaults changed"

expect_srt "srt://source.example:9000" 9000 caller
expect_srt "srt://source.example:9001?mode=caller&latency=120000" 9001 caller
expect_srt "srt://[2001:db8::1]:9002?mode=rendezvous" 9002 rendezvous
reject_srt "srt://127.0.0.1:1023"
reject_srt "srt://127.0.0.1:09000"
reject_srt "srt://127.0.0.1:9000/path"
reject_srt "srt://127.0.0.1:9000#fragment"
reject_srt "srt://127.0.0.1:9000?mode=listener&mode=caller"
reject_srt "srt://127.0.0.1:9000?mode=unsupported"
reject_srt "srt://127.0.0.1:9000?mode=listener?latency=1"

_LOAD_SETTINGS_ORIGINAL[SRT_URL]=srt://invalid
_LOAD_SETTINGS_ORIGINAL_SET[SRT_URL]=1
_LOAD_SETTINGS_ORIGINAL[CHANNEL_ENABLED]=false
_LOAD_SETTINGS_ORIGINAL_SET[CHANNEL_ENABLED]=1
load_channel_settings 1
[[ ${SRT_URL_VALID} == false ]] || fail "disabled legacy channel did not tolerate its unused invalid SRT URL"

echo "settings port tests passed"

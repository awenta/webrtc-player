#!/usr/bin/env bash
set -Eeuo pipefail

image=${1:-webrtc-player:port-config-test}
host_container=webrtc-player-port-test
bridge_container=webrtc-player-port-bridge-test

cleanup() {
    local status=$? container
    if (( status != 0 )); then
        for container in "${host_container}" "${bridge_container}"; do
            if docker inspect "${container}" >/dev/null 2>&1; then
                printf '\n--- %s state ---\n' "${container}" >&2
                docker inspect "${container}" --format '{{json .State}}' >&2 || true
                printf '%s\n' "--- ${container} logs ---" >&2
                docker logs --tail 300 "${container}" >&2 || true
            fi
        done
    fi
    docker rm -f "${host_container}" "${bridge_container}" >/dev/null 2>&1 || true
    return "${status}"
}
trap cleanup EXIT
cleanup

docker run -d --name "${host_container}" -e NETWORK_MODE=host \
    -e HOSTNAME=webrtc-player-unresolvable.invalid \
    -p 18088:8088/tcp "${image}" >/dev/null
WEBRTC_PLAYER_TEST_URL=http://127.0.0.1:18088 \
    WEBRTC_PLAYER_TEST_MODE=host \
    node tests/container-port-config.test.mjs

ingest_ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "${host_container}")
[[ -n ${ingest_ip} ]]
docker exec "${host_container}" sh -c "env \
    INPUT_MODE=rtp AUDIO_ENABLED=true RTP_BIND_IP=${ingest_ip} \
    VIDEO_PORT=6200 AUDIO_PORT=6201 VIDEO_RTCP_PORT=6202 AUDIO_RTCP_PORT=6203 \
    SRT_RELAY_VIDEO_PORT=16000 SRT_RELAY_AUDIO_PORT=16001 \
    SRT_RELAY_VIDEO_RTCP_PORT=16002 SRT_RELAY_AUDIO_RTCP_PORT=16003 \
    JANUS_VIDEO_PORT=26000 JANUS_AUDIO_PORT=26001 \
    JANUS_VIDEO_RTCP_PORT=26002 JANUS_AUDIO_RTCP_PORT=26003 \
    INPUT_TIMEOUT_MS=5000 RTP_PACING_US=100 \
    SELECTOR_STATUS_PATH=/tmp/occupied-selector.status \
    /usr/local/bin/input-selector >/tmp/occupied-selector.log 2>&1 &"
sleep 1
docker exec "${host_container}" sh -c \
    "grep -qi ':1838 ' /proc/net/udp || { cat /tmp/occupied-selector.log >&2; exit 1; }"
WEBRTC_PLAYER_TEST_URL=http://127.0.0.1:18088 \
    WEBRTC_PLAYER_TEST_MODE=host \
    WEBRTC_PLAYER_TEST_OCCUPIED_PORT=true \
    node tests/container-port-config.test.mjs

docker run -d --name "${bridge_container}" -p 18089:8088/tcp "${image}" >/dev/null
WEBRTC_PLAYER_TEST_URL=http://127.0.0.1:18089 \
    WEBRTC_PLAYER_TEST_MODE=bridge \
    node tests/container-port-config.test.mjs

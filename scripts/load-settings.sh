#!/bin/bash
# shellcheck shell=bash

readonly RUNTIME_SETTINGS_DIR=/config/settings

load_runtime_setting() {
    local name="$1" value
    if [[ -r "${RUNTIME_SETTINGS_DIR}/${name}" ]]; then
        IFS= read -r value <"${RUNTIME_SETTINGS_DIR}/${name}" || true
        printf -v "${name}" '%s' "${value}"
        export "${name?}"
    fi
}

for runtime_setting in \
    INPUT_MODE SRT_URL SRT_AUDIO AUDIO_ENABLED SRT_COLOR_MODE \
    SRT_PASSPHRASE SRT_PBKEYLEN VIDEO_BITRATE VIDEO_PRESET AUDIO_BITRATE \
    MAX_WIDTH MAX_HEIGHT MAX_FPS INPUT_TIMEOUT_MS STREAM_ID PUBLIC_IP; do
    load_runtime_setting "${runtime_setting}"
done

unset runtime_setting

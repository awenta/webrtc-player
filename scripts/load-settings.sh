#!/bin/bash
# shellcheck shell=bash

RUNTIME_SETTINGS_DIR=/config/settings
CHANNEL_SETTINGS_DIR=/config/channels
GLOBAL_SETTINGS_FILE=/config/global.conf
NETWORK_SETTINGS_FILE=/run/webrtc-player/network.env

if [[ $(declare -p _LOAD_SETTINGS_ORIGINAL 2>/dev/null) != declare\ -A* ||
        $(declare -p _LOAD_SETTINGS_ORIGINAL_SET 2>/dev/null) != declare\ -A* ]]; then
    unset _LOAD_SETTINGS_ORIGINAL _LOAD_SETTINGS_ORIGINAL_SET
    declare -gA _LOAD_SETTINGS_ORIGINAL=()
    declare -gA _LOAD_SETTINGS_ORIGINAL_SET=()
    for _load_settings_name in \
        CHANNEL_NAME CHANNEL_ENABLED INPUT_MODE SRT_URL SRT_AUDIO AUDIO_ENABLED \
        SRT_COLOR_MODE VIDEO_PRESET VIDEO_BITRATE VIDEO_BUFFER_SIZE AUDIO_BITRATE MAX_WIDTH MAX_HEIGHT \
        MAX_FPS INPUT_TIMEOUT_MS SRT_PBKEYLEN SRT_PASSPHRASE PUBLIC_IP \
        MANAGEMENT_INTERFACE INGEST_INTERFACE WEBRTC_INTERFACE NETWORK_MODE \
        SRT_PASSPHRASE_FILE; do
        if [[ -v ${_load_settings_name} ]]; then
            _LOAD_SETTINGS_ORIGINAL["${_load_settings_name}"]="${!_load_settings_name}"
            _LOAD_SETTINGS_ORIGINAL_SET["${_load_settings_name}"]=1
        fi
    done
    unset _load_settings_name
fi

_load_legacy_setting() {
    local name="$1" value
    [[ -r "${RUNTIME_SETTINGS_DIR}/${name}" ]] || return 1
    IFS= read -r value <"${RUNTIME_SETTINGS_DIR}/${name}" || [[ -n ${value} ]] || value=
    [[ ${value} != *$'\r'* && ${value} != *$'\n'* ]] || return 1
    REPLY=${value}
}

_load_channel_fallback() {
    local channel_id="$1" name="$2" fallback="$3"
    REPLY=${fallback}
    [[ ${channel_id} == 1 ]] || return 0
    if [[ -v _LOAD_SETTINGS_ORIGINAL_SET["${name}"] &&
            ${_LOAD_SETTINGS_ORIGINAL["${name}"]} != *$'\r'* &&
            ${_LOAD_SETTINGS_ORIGINAL["${name}"]} != *$'\n'* ]]; then
        REPLY=${_LOAD_SETTINGS_ORIGINAL["${name}"]}
    fi
    _load_legacy_setting "${name}" && return 0
    return 0
}

_load_channel_file() {
    local channel_id="$1" line key value
    declare -gA _LOAD_SETTINGS_CHANNEL_VALUES=()
    [[ -r "${CHANNEL_SETTINGS_DIR}/channel-${channel_id}.conf" ]] || return 0
    while IFS= read -r line || [[ -n ${line} ]]; do
        [[ ${line} == *=* && ${line} != *$'\r'* && ${line} != *$'\n'* ]] || continue
        key=${line%%=*}
        value=${line#*=}
        case ${key} in
            CHANNEL_NAME|CHANNEL_ENABLED|INPUT_MODE|SRT_URL|SRT_AUDIO|AUDIO_ENABLED|SRT_COLOR_MODE|VIDEO_PRESET|VIDEO_BITRATE|VIDEO_BUFFER_SIZE|AUDIO_BITRATE|MAX_WIDTH|MAX_HEIGHT|MAX_FPS|INPUT_TIMEOUT_MS|SRT_PBKEYLEN|SRT_PASSPHRASE)
                _LOAD_SETTINGS_CHANNEL_VALUES["${key}"]=${value}
                ;;
        esac
    done <"${CHANNEL_SETTINGS_DIR}/channel-${channel_id}.conf"
}

_resolve_channel_setting() {
    local channel_id="$1" name="$2" fallback="$3"
    if [[ -v _LOAD_SETTINGS_CHANNEL_VALUES["${name}"] ]]; then
        REPLY=${_LOAD_SETTINGS_CHANNEL_VALUES["${name}"]}
    else
        _load_channel_fallback "${channel_id}" "${name}" "${fallback}"
    fi
}

load_global_settings() {
    local line key value public_ip=
    local management_interface=auto ingest_interface=auto webrtc_interface=auto

    if [[ -v _LOAD_SETTINGS_ORIGINAL_SET[PUBLIC_IP] &&
            ${_LOAD_SETTINGS_ORIGINAL[PUBLIC_IP]} != *$'\r'* &&
            ${_LOAD_SETTINGS_ORIGINAL[PUBLIC_IP]} != *$'\n'* ]]; then
        public_ip=${_LOAD_SETTINGS_ORIGINAL[PUBLIC_IP]}
    fi
    if _load_legacy_setting PUBLIC_IP; then
        public_ip=${REPLY}
    fi
    for key in MANAGEMENT_INTERFACE INGEST_INTERFACE WEBRTC_INTERFACE; do
        if [[ -v _LOAD_SETTINGS_ORIGINAL_SET["${key}"] &&
                ${_LOAD_SETTINGS_ORIGINAL["${key}"]} != *$'\r'* &&
                ${_LOAD_SETTINGS_ORIGINAL["${key}"]} != *$'\n'* ]]; then
            case ${key} in
                MANAGEMENT_INTERFACE) management_interface=${_LOAD_SETTINGS_ORIGINAL["${key}"]} ;;
                INGEST_INTERFACE) ingest_interface=${_LOAD_SETTINGS_ORIGINAL["${key}"]} ;;
                WEBRTC_INTERFACE) webrtc_interface=${_LOAD_SETTINGS_ORIGINAL["${key}"]} ;;
            esac
        fi
    done
    if [[ -r ${GLOBAL_SETTINGS_FILE} ]]; then
        while IFS= read -r line || [[ -n ${line} ]]; do
            [[ ${line} == *=* && ${line} != *$'\r'* && ${line} != *$'\n'* ]] || continue
            key=${line%%=*}
            value=${line#*=}
            case ${key} in
                PUBLIC_IP) public_ip=${value} ;;
                MANAGEMENT_INTERFACE) management_interface=${value} ;;
                INGEST_INTERFACE) ingest_interface=${value} ;;
                WEBRTC_INTERFACE) webrtc_interface=${value} ;;
            esac
        done <"${GLOBAL_SETTINGS_FILE}"
    fi
    PUBLIC_IP=${public_ip}
    MANAGEMENT_INTERFACE=${management_interface}
    INGEST_INTERFACE=${ingest_interface}
    WEBRTC_INTERFACE=${webrtc_interface}
    export PUBLIC_IP MANAGEMENT_INTERFACE INGEST_INTERFACE WEBRTC_INTERFACE
}

load_network_settings() {
    local line key value
    local network_mode= default_interface= management_selector= management_name= management_ip=
    local ingest_selector= ingest_name= ingest_ip= webrtc_selector= webrtc_name= webrtc_ip=

    [[ -r ${NETWORK_SETTINGS_FILE} ]] || return 1
    while IFS= read -r line || [[ -n ${line} ]]; do
        [[ ${line} == *=* && ${line} != *$'\r'* && ${line} != *$'\n'* ]] || return 1
        key=${line%%=*}
        value=${line#*=}
        case ${key} in
            NETWORK_MODE) network_mode=${value} ;;
            DEFAULT_INTERFACE) default_interface=${value} ;;
            MANAGEMENT_SELECTOR) management_selector=${value} ;;
            MANAGEMENT_INTERFACE_NAME) management_name=${value} ;;
            MANAGEMENT_IP) management_ip=${value} ;;
            INGEST_SELECTOR) ingest_selector=${value} ;;
            INGEST_INTERFACE_NAME) ingest_name=${value} ;;
            INGEST_IP) ingest_ip=${value} ;;
            WEBRTC_SELECTOR) webrtc_selector=${value} ;;
            WEBRTC_INTERFACE_NAME) webrtc_name=${value} ;;
            WEBRTC_IP) webrtc_ip=${value} ;;
            *) return 1 ;;
        esac
    done <"${NETWORK_SETTINGS_FILE}"
    [[ ${network_mode} == host || ${network_mode} == bridge ]] || return 1
    for value in "${default_interface}" "${management_name}" "${ingest_name}" "${webrtc_name}"; do
        [[ ${value} =~ ^[A-Za-z0-9_.:-]{1,15}$ ]] || return 1
    done
    for value in "${management_selector}" "${ingest_selector}" "${webrtc_selector}"; do
        [[ ${value} == auto || ${value} =~ ^[A-Za-z0-9_.:-]{1,15}$ ]] || return 1
    done
    for value in "${management_ip}" "${ingest_ip}" "${webrtc_ip}"; do
        [[ ${value} =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || return 1
    done

    NETWORK_MODE=${network_mode}
    DEFAULT_INTERFACE=${default_interface}
    MANAGEMENT_SELECTOR=${management_selector}
    MANAGEMENT_INTERFACE_NAME=${management_name}
    MANAGEMENT_IP=${management_ip}
    INGEST_SELECTOR=${ingest_selector}
    INGEST_INTERFACE_NAME=${ingest_name}
    INGEST_IP=${ingest_ip}
    WEBRTC_SELECTOR=${webrtc_selector}
    WEBRTC_INTERFACE_NAME=${webrtc_name}
    WEBRTC_IP=${webrtc_ip}
    RTP_BIND_IP=${ingest_ip}
    export NETWORK_MODE DEFAULT_INTERFACE MANAGEMENT_SELECTOR MANAGEMENT_INTERFACE_NAME MANAGEMENT_IP \
        INGEST_SELECTOR INGEST_INTERFACE_NAME INGEST_IP WEBRTC_SELECTOR WEBRTC_INTERFACE_NAME WEBRTC_IP \
        RTP_BIND_IP
}

load_channel_settings() {
    local channel_id="$1" channel_name srt_url
    local external_base relay_base janus_base srt_port

    [[ ${channel_id} =~ ^[1-5]$ ]] || return 2
    external_base=$((5004 + 4 * (channel_id - 1)))
    relay_base=$((15004 + 4 * (channel_id - 1)))
    janus_base=$((25004 + 4 * (channel_id - 1)))
    srt_port=$((9000 + channel_id - 1))
    channel_name="Channel ${channel_id}"
    srt_url="srt://0.0.0.0:${srt_port}?mode=listener&latency=120000"

    _load_channel_file "${channel_id}"

    _resolve_channel_setting "${channel_id}" CHANNEL_NAME "${channel_name}"; CHANNEL_NAME=${REPLY}
    _resolve_channel_setting "${channel_id}" CHANNEL_ENABLED true; CHANNEL_ENABLED=${REPLY}
    _resolve_channel_setting "${channel_id}" INPUT_MODE auto; INPUT_MODE=${REPLY}
    _resolve_channel_setting "${channel_id}" SRT_URL "${srt_url}"; SRT_URL=${REPLY}
    _resolve_channel_setting "${channel_id}" SRT_AUDIO true; SRT_AUDIO=${REPLY}
    _resolve_channel_setting "${channel_id}" AUDIO_ENABLED true; AUDIO_ENABLED=${REPLY}
    _resolve_channel_setting "${channel_id}" SRT_COLOR_MODE auto; SRT_COLOR_MODE=${REPLY}
    _resolve_channel_setting "${channel_id}" VIDEO_PRESET veryfast; VIDEO_PRESET=${REPLY}
    _resolve_channel_setting "${channel_id}" VIDEO_BITRATE 6M; VIDEO_BITRATE=${REPLY}
    _resolve_channel_setting "${channel_id}" VIDEO_BUFFER_SIZE 2M; VIDEO_BUFFER_SIZE=${REPLY}
    _resolve_channel_setting "${channel_id}" AUDIO_BITRATE 128k; AUDIO_BITRATE=${REPLY}
    _resolve_channel_setting "${channel_id}" MAX_WIDTH 1920; MAX_WIDTH=${REPLY}
    _resolve_channel_setting "${channel_id}" MAX_HEIGHT 1080; MAX_HEIGHT=${REPLY}
    _resolve_channel_setting "${channel_id}" MAX_FPS 60; MAX_FPS=${REPLY}
    _resolve_channel_setting "${channel_id}" INPUT_TIMEOUT_MS 5000; INPUT_TIMEOUT_MS=${REPLY}
    _resolve_channel_setting "${channel_id}" SRT_PBKEYLEN 16; SRT_PBKEYLEN=${REPLY}
    _resolve_channel_setting "${channel_id}" SRT_PASSPHRASE ''; SRT_PASSPHRASE=${REPLY}

    CHANNEL_ID=${channel_id}
    STREAM_ID=${channel_id}
    VIDEO_PORT=${external_base}
    AUDIO_PORT=$((external_base + 1))
    VIDEO_RTCP_PORT=$((external_base + 2))
    AUDIO_RTCP_PORT=$((external_base + 3))
    SRT_RELAY_VIDEO_PORT=${relay_base}
    SRT_RELAY_AUDIO_PORT=$((relay_base + 1))
    SRT_RELAY_VIDEO_RTCP_PORT=$((relay_base + 2))
    SRT_RELAY_AUDIO_RTCP_PORT=$((relay_base + 3))
    JANUS_VIDEO_PORT=${janus_base}
    JANUS_AUDIO_PORT=$((janus_base + 1))
    JANUS_VIDEO_RTCP_PORT=$((janus_base + 2))
    JANUS_AUDIO_RTCP_PORT=$((janus_base + 3))
    SRT_PORT=${srt_port}
    SRT_PUBLIC_PORT=${srt_port}
    HTTP_PORT=8088
    ICE_PORTS=20000-20100/udp

    if [[ ${channel_id} == 1 && -v _LOAD_SETTINGS_ORIGINAL_SET[SRT_PASSPHRASE_FILE] &&
            ${_LOAD_SETTINGS_ORIGINAL[SRT_PASSPHRASE_FILE]} != *$'\r'* &&
            ${_LOAD_SETTINGS_ORIGINAL[SRT_PASSPHRASE_FILE]} != *$'\n'* ]]; then
        SRT_PASSPHRASE_FILE=${_LOAD_SETTINGS_ORIGINAL[SRT_PASSPHRASE_FILE]}
        export SRT_PASSPHRASE_FILE
    else
        unset SRT_PASSPHRASE_FILE
    fi

    export CHANNEL_ID CHANNEL_NAME CHANNEL_ENABLED STREAM_ID \
        INPUT_MODE SRT_URL SRT_AUDIO AUDIO_ENABLED SRT_COLOR_MODE VIDEO_PRESET \
        VIDEO_BITRATE VIDEO_BUFFER_SIZE AUDIO_BITRATE MAX_WIDTH MAX_HEIGHT MAX_FPS INPUT_TIMEOUT_MS \
        SRT_PBKEYLEN SRT_PASSPHRASE VIDEO_PORT AUDIO_PORT VIDEO_RTCP_PORT \
        AUDIO_RTCP_PORT SRT_RELAY_VIDEO_PORT SRT_RELAY_AUDIO_PORT \
        SRT_RELAY_VIDEO_RTCP_PORT SRT_RELAY_AUDIO_RTCP_PORT JANUS_VIDEO_PORT \
        JANUS_AUDIO_PORT JANUS_VIDEO_RTCP_PORT JANUS_AUDIO_RTCP_PORT SRT_PORT \
        SRT_PUBLIC_PORT HTTP_PORT ICE_PORTS
}

load_global_settings
if [[ -r ${NETWORK_SETTINGS_FILE} ]]; then
    load_network_settings
fi
load_channel_settings "${CHANNEL_ID:-1}"

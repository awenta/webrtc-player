#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

readonly REQUEST_FILE=/run/webrtc-player/config-apply.request
readonly PROCESSING_FILE=/run/webrtc-player/config-apply.processing
readonly RESULT_FILE=/run/webrtc-player/config-apply.result
readonly S6_SVC=/package/admin/s6/command/s6-svc
publish_result() {
    local generation="$1" status="$2"
    local temporary="${RESULT_FILE}.tmp"
    printf 'generation=%s\nstatus=%s\n' "${generation}" "${status}" >"${temporary}"
    chown player:player "${temporary}"
    mv -f "${temporary}" "${RESULT_FILE}"
}

while true; do
    if [[ ! -f "${REQUEST_FILE}" ]]; then
        sleep 0.25
        continue
    fi

    if ! mv "${REQUEST_FILE}" "${PROCESSING_FILE}" 2>/dev/null; then
        sleep 0.1
        continue
    fi
    generation=unknown
    channel=
    global_changed=false
    while IFS='=' read -r key value; do
        case "${key}" in
            generation) generation="${value}" ;;
            channel) channel="${value}" ;;
            global) global_changed="${value}" ;;
        esac
    done <"${PROCESSING_FILE}"
    if [[ ! "${channel}" =~ ^[0-5]$ || ( "${channel}" == 0 && "${global_changed}" != true ) ]]; then
        publish_result "${generation}" error
        echo "[config] invalid channel in generation ${generation}" >&2
        rm -f "${PROCESSING_FILE}"
        continue
    fi
    if [[ "${channel}" == 0 ]]; then
        echo "[config] applying global generation ${generation}"
    else
        echo "[config] applying generation ${generation} to channel ${channel}"
        selector_service="input-selector-${channel}"
        relay_service="srt-relay-${channel}"
        "${S6_SVC}" -d "/run/service/${selector_service}"
        "${S6_SVC}" -d "/run/service/${relay_service}"
    fi

    apply_status=ok
    if [[ "${global_changed}" == "true" ]]; then
        "${S6_SVC}" -d /run/service/janus
        if ! /etc/cont-init.d/10-configure; then
            apply_status=error
        fi
        "${S6_SVC}" -u /run/service/janus
    fi

    if [[ "${channel}" != 0 ]]; then
        "${S6_SVC}" -u "/run/service/${selector_service}"
        "${S6_SVC}" -u "/run/service/${relay_service}"
    fi
    publish_result "${generation}" "${apply_status}"
    echo "[config] generation ${generation} status=${apply_status}"
    rm -f "${PROCESSING_FILE}"
done

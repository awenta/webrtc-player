#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

readonly REQUEST_FILE=/run/webrtc-player/config-apply.request
readonly PROCESSING_FILE=/run/webrtc-player/config-apply.processing
readonly RESULT_FILE=/run/webrtc-player/config-apply.result
readonly S6_SVC=/package/admin/s6/command/s6-svc
readonly SERVICES=(stats srt-relay input-selector janus)

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
    IFS= read -r generation <"${PROCESSING_FILE}" || generation=unknown
    echo "[config] applying generation ${generation}"

    for service in "${SERVICES[@]}"; do
        "${S6_SVC}" -d "/run/service/${service}" || true
    done
    sleep 1

    if /etc/cont-init.d/10-configure; then
        publish_result "${generation}" ok
        echo "[config] generation ${generation} applied"
    else
        publish_result "${generation}" error
        echo "[config] generation ${generation} failed validation" >&2
    fi

    for service in janus input-selector srt-relay stats; do
        "${S6_SVC}" -u "/run/service/${service}" || true
    done
    rm -f "${PROCESSING_FILE}"
done

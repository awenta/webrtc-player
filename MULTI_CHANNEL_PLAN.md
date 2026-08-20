# Multi-Channel Implementation Plan

This temporary checklist tracks implementation of five concurrent named channels and comparable input/output monitoring.

## Locked Decisions

- Five fixed channel slots can run concurrently.
- Stream IDs are fixed to 1-5.
- SRT listener ports are fixed to 9000-9004.
- Direct RTP/RTCP ports are fixed to 5004-5023 in four-port channel blocks.
- One Janus, Nginx, WebSocket endpoint, ICE range, config API, and status API are shared.
- The UI displays one selected channel at a time; the browser player is preview only.
- All five channels start enabled unless explicitly disabled in persisted configuration.
- Browser preview can be disabled locally, tearing down its Janus session and browser media processing.
- Output telemetry represents all clients served by Janus.
- Input monitoring is media-level; native SRT retransmission/RTT telemetry is out of scope.
- Input, Processing, and Output use contextual editors.
- Each channel card owns its channel configuration dialog; the top-level settings dialog contains global configuration only.
- Management, ingest, and WebRTC default to the primary IPv4 interface and can select independent host interfaces.

## Progress

- [x] Introduce atomic global and per-channel configuration with channel-1 migration.
- [x] Provision five Janus mountpoints and deterministic external/internal ports.
- [x] Supervise independent selector and SRT relay processes for each channel.
- [x] Publish version-2 per-channel status and health information.
- [x] Add per-channel input and normalized-media counters.
- [x] Add loopback-only Janus aggregate viewer/output telemetry.
- [x] Add selected-channel UI navigation, preview switching, URLs, and contextual editors.
- [x] Verify five-channel isolation, configuration impact, telemetry, and resource behavior.
- [x] Add automatic default-route discovery and independent management, ingest, and WebRTC interface roles.
- [x] Update documentation and changelog.

## Port Map

| Channel | Stream ID | SRT | Direct RTP/RTCP | Relay RTP/RTCP | Janus RTP/RTCP |
|---|---:|---:|---:|---:|---:|
| 1 | 1 | 9000 | 5004-5007 | 15004-15007 | 25004-25007 |
| 2 | 2 | 9001 | 5008-5011 | 15008-15011 | 25008-25011 |
| 3 | 3 | 9002 | 5012-5015 | 15012-15015 | 25012-25015 |
| 4 | 4 | 9003 | 5016-5019 | 15016-15019 | 25016-25019 |
| 5 | 5 | 9004 | 5020-5023 | 15020-15023 | 25020-25023 |

## Acceptance Criteria

- Five enabled channels ingest and serve independently without cross-talk.
- Selecting a channel changes only the visible stats, controls, preview, and URLs.
- Enabling or reconfiguring one channel does not interrupt unrelated channels.
- Output metrics include active viewers and aggregate Janus delivery quality.
- Unknown or inapplicable metrics are labelled as such rather than reported as zero.
- Existing persisted settings migrate to Channel 1; all channels without an explicit setting start enabled.
- The container remains non-root/read-only and the Janus Admin endpoint is loopback-only.
- Production host networking binds each externally reachable service only to its selected IPv4 role address.

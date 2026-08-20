# WebRTC Player

An embeddable, low-latency five-channel WebRTC player powered by Janus Gateway. Each channel accepts either direct H.264/Opus RTP or an SRT MPEG-TS contribution feed, then serves the stream to browsers through WebRTC from one container.

```text
SRT (H.264/HEVC + common audio) -> FFmpeg -> H.264/Opus RTP -+
Direct H.264/Opus RTP ----------------------------------------+-> Janus -> WebRTC -> Browser
                                                                  ^
                                            Nginx player UI + WebSocket proxy
```

## Components

The image uses reviewed, immutable upstream revisions rather than floating branches. Ubuntu packages use a dated snapshot after the minimal CA-certificate bootstrap needed to reach that HTTPS snapshot.

| Component | Pinned version |
|---|---|
| Ubuntu | 26.04 LTS image digest and 2026-08-20 package snapshot |
| Janus Gateway | 1.4.1 |
| FFmpeg | 9.0.1 |
| Haivision SRT | 1.5.6 |
| libSRTP | 2.8.0 |
| Nginx mainline | 1.31.4 |
| s6-overlay | 3.2.3.2 |
| x264 | Immutable commit in `Dockerfile` |

The runtime image contains no compiler or source tree. Janus, Nginx, and the optional SRT relay run as the unprivileged `player` user under s6 supervision.

## Quick Start

Build and start with automatic RTP/SRT input selection:

```bash
docker compose up --build
```

Open <http://localhost:8088/?stream=1>, or embed it:

```html
<iframe src="http://localhost:8088/?stream=1" allow="autoplay" style="width:640px;height:360px;border:none"></iframe>
```

The player begins muted to satisfy browser autoplay rules. Its native controls can enable audio. All five channels start enabled unless a persisted channel setting explicitly disables one.

| Channel | Player | SRT listener | Direct RTP/RTCP |
|---|---|---:|---:|
| 1 | `?stream=1` | 9000 | 5004-5007 |
| 2 | `?stream=2` | 9001 | 5008-5011 |
| 3 | `?stream=3` | 9002 | 5012-5015 |
| 4 | `?stream=4` | 9003 | 5016-5019 |
| 5 | `?stream=5` | 9004 | 5020-5023 |

## Deploy a Published Image

GitHub Actions publishes the default branch and version tags to GitHub Container Registry as a `linux/amd64` image:

```text
ghcr.io/awenta/webrtc-player
```

Download `compose.deploy.yml` on an x86-64 Docker host, then start the published image without cloning or building the repository:

```bash
curl -fsSLO https://raw.githubusercontent.com/awenta/webrtc-player/claude/webrtc-video-player-iframe-phPMa/compose.deploy.yml
docker compose -f compose.deploy.yml pull
docker compose -f compose.deploy.yml up -d
```

The default image is `latest`. For a reproducible deployment, select a version or immutable SHA tag:

```bash
WEBRTC_PLAYER_IMAGE=ghcr.io/awenta/webrtc-player:v1.0.0 docker compose -f compose.deploy.yml up -d
```

To upgrade while preserving channel configuration:

```bash
docker compose -f compose.deploy.yml pull
docker compose -f compose.deploy.yml up -d
```

The `player-config` volume survives image upgrades and container recreation. Do not use `docker compose down -v` unless the persisted channel/global settings should be deleted.

The repository is public, but a newly created GHCR package may need to be changed to **Public** once in the package settings before anonymous pulls work. If the package remains private, authenticate the deployment host with a GitHub token that has `read:packages`:

```bash
printf '%s' "$GITHUB_TOKEN" | docker login ghcr.io -u YOUR_GITHUB_USER --password-stdin
```

Publishing behavior:

- A default-branch build publishes `latest` and `sha-<commit>`.
- A tag such as `v1.2.3` publishes `v1.2.3`, `1.2.3`, `1.2`, and `sha-<commit>`.
- The workflow can also be started manually from the GitHub Actions page.

## Automatic Input Selection

`INPUT_MODE=auto` is the default. The container listens for supported direct RTP and SRT at the same time, validates incoming RTP, and locks onto the first source that delivers a valid H.264 video sequence. The selected input is forwarded to the existing Janus WebRTC output; the WebRTC stream ID and browser workflow do not change.

The selector keeps the active source until its video packets stop for `INPUT_TIMEOUT_MS` (default 5000 ms), then returns to waiting for either input. Simultaneous sources are never mixed. Use `INPUT_MODE=rtp` or `INPUT_MODE=srt` only when an operator needs to restrict accepted inputs.

Automatic mode accepts:

- Direct H.264 RTP PT 96 and Opus RTP PT 111 on the documented RTP/RTCP ports
- SRT MPEG-TS callers on UDP 9000, with H.264/HEVC and supported audio normalized by FFmpeg
- An SRT caller URL configured through `SRT_URL`; the URL supplies the destination that cannot be discovered automatically

When an SRT passphrase or passphrase file is configured, automatic mode disables unauthenticated direct RTP so it cannot bypass SRT encryption.

## Monitoring and Setup UI

The root page combines a responsive signal monitor with per-channel input/output setup. Select one of the five channels to update the monitor, connection URLs, settings, and optional browser preview. The monitor shows only data available from the running application:

- Input mode, transport state, listener/caller address, and RTP/RTCP ports
- SRT output cadence, media bitrate, processing speed, and application CPU/memory use
- Janus, FFmpeg, and Nginx process state plus system load and monitor uptime
- This browser's WebRTC resolution, frame rate, video/audio receive bitrate, codec, interval packet loss, decoded/dropped frames, jitter, and selected ICE path

Runtime status is refreshed from `GET /api/status`; browser receive statistics come directly from `RTCPeerConnection.getStats()`. Direct RTP input bitrate and sender details are not displayed because Janus does not currently expose those values to the monitor.

Select **Disable preview** to stop browser media processing without stopping the channel. This immediately destroys the page's Janus session, closes its WebSocket and peer connection, detaches the media element, stops preview statistics collection, and persists the preference in that browser. Server aggregate output monitoring continues and **Enable preview** reconnects the selected channel.

Select **Config** on a channel card to open all settings for that channel:

- Automatic, RTP-only, or SRT-only input acceptance and source timeout
- SRT listener/caller URL, expected audio, color handling, and optional encryption
- Transcode bitrate, x264 preset, maximum dimensions, and frame rate
- Direct-RTP audio and the channel's fixed WebRTC/port allocation

The top-level **Global settings** button contains only settings shared by every channel, currently the public IP advertised by Janus. Applying channel settings validates and persists that channel, then briefly restarts only its selector and SRT relay. Applying global settings leaves all channel processes running and restarts the shared Janus service. The browser reconnects automatically when its selected output is affected.

Persisted UI settings take precedence over matching environment variables on later container starts; use `docker compose down -v` to remove them and return to environment/default values.

Published HTTP, SRT, RTP/RTCP, and ICE ports are shown but remain read-only because Docker port mappings cannot be changed safely from inside the container. The connection guide displays copy-ready SRT, RTP, player, WebSocket, and iframe values based on the host currently used to open the UI.

The configuration endpoint is intentionally simple and has no user-account system. Do not expose it to untrusted networks without access control in the deployment reverse proxy.

The Janus Streaming mountpoint uses two relay helper threads so its RTP receiver can keep draining bursty video packets while WebRTC forwarding runs independently. Its RTP/RTCP sockets request a 4 MiB receive buffer, and the input selector drains incoming bursts into a bounded queue before pacing video RTP toward Janus in small batches. Batched pacing avoids coarse virtual-machine timer rounding while keeping each Janus burst small, without privileged host kernel tuning.

## SRT Listener

The default automatic mode runs one SRT listener per channel on UDP ports 9000-9004:

```bash
docker compose up --build
```

Send MPEG-TS to the selected channel's port, for example Channel 1:

```text
srt://<container-host>:9000?mode=caller&latency=120000
```

The container's matching default input is:

```text
srt://0.0.0.0:9000?mode=listener&latency=120000
```

## SRT Caller

Make the container connect to a remote SRT listener:

```bash
SRT_URL='srt://source.example.net:9000?mode=caller&latency=120000' \
docker compose up --build
```

On PowerShell:

```powershell
$env:SRT_URL = 'srt://source.example.net:9000?mode=caller&latency=120000'
docker compose up --build
```

FFmpeg exits when an SRT connection ends; s6 restarts it with a short delay so listener and caller modes both reconnect.

SRT packets are drained on a dedicated bounded FFmpeg input queue so codec initialization and transient encoder stalls do not cause transport-level packet loss.

File-based test senders must pace output in real time. VLC 3's SRT stream-output module can run a file faster than its timestamps, eventually filling the SRT receive queue and causing MPEG-TS corruption even on localhost. Use a real-time encoder or an FFmpeg sender with `-re` when evaluating packet and frame loss.

## Supported SRT Media

SRT is expected to carry MPEG-TS with one selected video track and, by default, one selected audio track.

Video input includes:

- H.264/AVC and HEVC/H.265 Main/Main10
- 8-bit and 10-bit input
- Common 4:2:0, 4:2:2, and 4:4:4 pixel formats supported by FFmpeg
- Progressive and correctly flagged interlaced content
- SDR content by default, with an explicit HDR/HLG/PQ-to-SDR mode

All SRT video is decoded and normalized to constrained-baseline H.264 Level 4.2, 8-bit 4:2:0 at no more than 60 fps. Audio is normalized to stereo Opus at 48 kHz. This is required because HEVC is not broadly interoperable in browser WebRTC and unmodified high-bitrate H.264 keyframes can exceed the unprivileged UDP buffers between FFmpeg and Janus.

Common audio decoders include AAC, Opus, MP2/MP3, AC-3, E-AC-3, and PCM. Actual acceptance depends on the demuxer and decoders included in FFmpeg 9.0.1.

The default output ceiling is 1920x1080. CPU-only HEVC-to-H.264 at 1080p60 requires a suitably provisioned modern CPU; measure the `speed=` value in FFmpeg logs on the deployment hardware. Values below `1.0x` indicate the host cannot sustain real time.

### Color Modes

| `SRT_COLOR_MODE` | Behavior |
|---|---|
| `auto` | Default. Accepts common bit depths/chroma and converts to 8-bit 4:2:0 without expensive tone mapping. HDR color appearance may not be preserved. |
| `fast` | Same low-cost conversion path, intended for known SDR sources. |
| `hdr-to-sdr` | CPU-intensive zscale/Mobius conversion from HDR, HLG, or PQ to BT.709 SDR. |

Use `hdr-to-sdr` only when required and benchmark it at the target frame rate.

### Video-Only SRT

Set `SRT_AUDIO=false` if the transport stream has no audio track:

```bash
SRT_AUDIO=false docker compose up
```

When audio is enabled, a missing selected audio stream is treated as an input error and the relay retries. Select alternate tracks with `VIDEO_MAP` and `AUDIO_MAP`, for example `AUDIO_MAP=0:a:1`.

## Direct RTP Mode

Automatic mode listens for direct RTP by default. Send:

| Media | Destination | RTP payload |
|---|---|---|
| H.264 video | `rtp://<host>:5004` | PT 96, 90 kHz |
| Opus audio | `rtp://<host>:5005` | PT 111, 48 kHz stereo |

Optional RTCP sender reports use UDP 5006 for video and 5007 for audio. These ports are separate so video RTCP cannot collide with audio RTP.

Channels 2-5 use the subsequent four-port blocks shown in the Quick Start table.

Set `AUDIO_ENABLED=false` for a video-only direct RTP mountpoint.

If direct RTP and SRT arrive together, the first validated video source is selected and the other is ignored until the active input times out.

## Configuration

| Variable | Default | Description |
|---|---|---|
| `CHANNEL_ENABLED` | `true` | Enable the channel; persisted per-channel settings take precedence |
| `INPUT_MODE` | `auto` | Automatic RTP/SRT selection, or the `rtp`/`srt` restriction overrides |
| `INPUT_TIMEOUT_MS` | `5000` | Video silence before automatic mode releases the selected input |
| `RTP_PACING_US` | `100` | Minimum spacing between video RTP packets forwarded to Janus (0-2000 microseconds) |
| `STREAM_ID` | `1` | Positive Janus mountpoint ID |
| `VIDEO_PORT` | `5004` | Internal/direct H.264 RTP port |
| `AUDIO_PORT` | `5005` | Internal/direct Opus RTP port |
| `VIDEO_RTCP_PORT` | `5006` | Internal/direct H.264 RTCP port |
| `AUDIO_RTCP_PORT` | `5007` | Internal/direct Opus RTCP port |
| `AUDIO_ENABLED` | `true` | Enable audio in direct RTP mode |
| `PUBLIC_IP` | unset | IP advertised by Janus for one-to-one NAT |
| `SRT_URL` | listener on `0.0.0.0:9000` | Complete listener or caller URL |
| `SRT_AUDIO` | `true` | Expect and transcode an SRT audio stream |
| `SRT_COLOR_MODE` | `auto` | `auto`, `fast`, or `hdr-to-sdr` |
| `VIDEO_BITRATE` | `6M` | H.264 target and maximum bitrate |
| `VIDEO_BUFFER_SIZE` | `2M` | x264 VBV reservoir; bounds keyframe bursts before local RTP transport |
| `VIDEO_PRESET` | `veryfast` | x264 CPU/quality preset |
| `AUDIO_BITRATE` | `128k` | Opus bitrate |
| `MAX_WIDTH` | `1920` | Maximum output width |
| `MAX_HEIGHT` | `1080` | Maximum output height |
| `MAX_FPS` | `60` | Maximum output frame rate, from 1 through 60 |
| `VIDEO_MAP` | `0:v:0` | FFmpeg video stream selector |
| `AUDIO_MAP` | `0:a:0` | FFmpeg audio stream selector |
| `FFMPEG_LOG_LEVEL` | `warning` | FFmpeg logging level |
| `JANUS_ADMIN_KEY` | generated | Optional persistent Streaming plugin management key |
| `STREAM_SECRET` | generated | Optional persistent mountpoint edit/destroy secret |

`SRT_PORT` only controls the Docker Compose host-to-container port publication. Keep it consistent with the listener port in `SRT_URL`.

Environment variables and legacy `/config/settings` values migrate to Channel 1. The UI stores independent records for all five channels under `/config/channels`.

## SRT Encryption

Prefer a Docker secret over embedding a passphrase in `SRT_URL`, which can expose it in logs and deployment metadata:

```yaml
services:
  webrtc-player:
    environment:
      SRT_PASSPHRASE_FILE: /run/secrets/srt_passphrase
      SRT_PBKEYLEN: 32
    secrets:
      - srt_passphrase

secrets:
  srt_passphrase:
    file: ./srt-passphrase.txt
```

Valid passphrases contain 10 to 79 characters. Valid `SRT_PBKEYLEN` values are 16, 24, and 32.

FFmpeg requires the passphrase as a protocol option, so a privileged host operator or a process inspector inside the container can still read it from FFmpeg's command line. The secret mount keeps it out of Compose metadata and normal logs; restrict Docker and container-exec access accordingly. Passphrase files matching `*passphrase*.txt` are excluded from Git and the Docker build context by default.

## Janus Management Protection

Viewing a stream requires no browser-visible secret. At each startup, the container generates a random Streaming plugin admin key and mountpoint secret to prevent public WebSocket clients from creating, editing, or destroying mountpoints. Set `JANUS_ADMIN_KEY` and `STREAM_SECRET` explicitly only when an external trusted management client needs stable credentials; supported characters are letters, digits, `.`, `_`, and `-`.

Janus always receives media from the loopback input selector. With encrypted SRT, automatic mode disables the selector's external RTP listeners so published RTP ports cannot bypass authentication.

## NAT and Public Deployment

`PUBLIC_IP` must be the IP browsers use to reach this host. Leave it unset only when Janus interface candidates are directly reachable. For remote or cloud deployments:

```bash
PUBLIC_IP=203.0.113.10 docker compose up
```

Allow these ports through the host firewall:

- `8088/tcp`: player and Janus WebSocket proxy
- `9000-9004/udp`: SRT listeners for Channels 1-5
- `5004-5023/udp`: direct RTP/RTCP for Channels 1-5
- `20000-20100/udp`: Janus WebRTC ICE/DTLS/SRTP

Host networking can simplify ICE on Linux. Remove Compose port publishing when enabling `network_mode: host`.

Terminate TLS in a trusted reverse proxy for production so the player is served over HTTPS/WSS.

## Operations

The image health check verifies that s6 reports every enabled channel's selector and SRT relay as running, verifies the shared Janus, Nginx, and status services, and requests Nginx `/healthz`. A disconnected SRT listener remains healthy because automatic mode is ready to accept either supported input; the selected source is shown in the monitoring UI.

Useful commands:

```bash
docker compose ps
docker compose logs -f webrtc-player
docker compose exec webrtc-player /opt/media/bin/ffmpeg -version
docker compose exec webrtc-player /opt/janus/bin/janus --version
```

## Updating Dependencies

Versions, source commits, image digests, checksums, and the post-bootstrap Ubuntu package snapshot are deliberately pinned in `Dockerfile`. Update them manually as one reviewed change, rebuild without cache, and repeat the H.264/HEVC listener/caller playback tests before release.

The FFmpeg build enables GPL-licensed x264. Distributors are responsible for satisfying applicable open-source and codec patent/licensing obligations.

# Changelog

## [Unreleased]

- Automatically bind management, ingest, and WebRTC roles to the primary IPv4 interface with independent UI overrides for multi-interface Linux hosts.
- Publish versioned `linux/amd64` images to GHCR and provide a pull-based deployment Compose file.
- Split configuration into per-channel card dialogs and a global-only settings dialog with isolated service restarts.
- Enable all five channels by default and allow browser previews to be fully disconnected to save client and Janus resources.
- Add protected H.264/HEVC SRT listener and caller ingest with codec normalization, pinned media components, and a hardened multi-stage container.
- Add a responsive read-only signal monitor for ingest, processing, service, port, and browser WebRTC statistics.
- Make input selection automatic across supported direct RTP and SRT sources while keeping explicit restriction modes available.
- Add persistent in-dashboard input/output setup and copy-ready connection examples using the instance's actual host and ports.
- Report interval WebRTC packet loss separately from browser decoded/dropped frame counts.
- Use two Janus relay helper threads to prevent bursty video RTP from overflowing the WebRTC ingest socket.
- Enlarge Janus Streaming UDP receive buffers to absorb frame-sized RTP bursts without packet loss.
- Drain and pace selector-to-Janus RTP through a bounded queue to prevent burst-driven packet loss.
- Drain SRT on a dedicated FFmpeg input queue to avoid startup packet loss during encoder initialization.
- Batch RTP pacing and enlarge the selector queue to prevent keyframe bursts from causing loss or jitter.
- Bound x264 keyframe bursts with a smaller VBV reservoir so they fit local RTP socket buffers.

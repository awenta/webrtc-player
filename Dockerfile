# syntax=docker/dockerfile:1.7

ARG UBUNTU_IMAGE=ubuntu:26.04@sha256:2260313b31c8c011cd2eebe728008efac1b3982be73eb71348ea2648d2c0e09b

FROM ${UBUNTU_IMAGE} AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG TARGETARCH=amd64
ARG UBUNTU_SNAPSHOT=20260820T000000Z
ARG JANUS_COMMIT=85304a7a4bdb2719583ad53ae04130a8273dba6a
ARG FFMPEG_COMMIT=bf1b838f2ab88b4f8fd83443325c782ea0e0f7fa
ARG SRT_COMMIT=c63c311e88aa55e430e3b7d94b89d790994f88c4
ARG X264_COMMIT=c24e06c2e184345ceb33eb20a15d1024d9fd3497
ARG LIBSRTP_COMMIT=24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5
ARG NGINX_VERSION=1.31.4
ARG NGINX_SHA256=e6f20b644a17a643f059ae6467a1971fe2811587d025e071068753a1f1e3b3c3
ARG S6_OVERLAY_VERSION=3.2.3.2
ARG S6_NOARCH_SHA256=5379750ed30a84bbd2e2dd74847ba6b5bd29cd0b2e3ea2ec58049b57eb2eda12
ARG S6_X86_64_SHA256=e6befcc96a437a3831386ecfc51808c5d3e939dc5fe3c02ae9284599e8aa2408

RUN test "${TARGETARCH}" = "amd64" || (echo "Only linux/amd64 is currently supported" >&2; exit 1)

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && sed -i \
        -e "s|http://archive.ubuntu.com/ubuntu/|https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/|g" \
        -e "s|http://security.ubuntu.com/ubuntu/|https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && printf 'Acquire::Check-Valid-Until "false";\n' >/etc/apt/apt.conf.d/99snapshot \
    && apt-get update && apt-get install -y --no-install-recommends \
    autoconf \
    automake \
    build-essential \
    cmake \
    curl \
    gengetopt \
    git \
    libconfig-dev \
    libcurl4-openssl-dev \
    libglib2.0-dev \
    libjansson-dev \
    libmicrohttpd-dev \
    libnice-dev \
    libogg-dev \
    libopus-dev \
    libpcre2-dev \
    libsofia-sip-ua-dev \
    libsrtp2-dev \
    libssl-dev \
    libtool \
    libusrsctp-dev \
    libwebsockets-dev \
    libzimg-dev \
    nasm \
    ninja-build \
    pax-utils \
    pkg-config \
    xz-utils \
    yasm \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ADD --checksum=sha256:${S6_NOARCH_SHA256} \
    https://github.com/just-containers/s6-overlay/releases/download/v${S6_OVERLAY_VERSION}/s6-overlay-noarch.tar.xz \
    /tmp/s6-overlay-noarch.tar.xz
ADD --checksum=sha256:${S6_X86_64_SHA256} \
    https://github.com/just-containers/s6-overlay/releases/download/v${S6_OVERLAY_VERSION}/s6-overlay-x86_64.tar.xz \
    /tmp/s6-overlay-x86_64.tar.xz
RUN mkdir -p /opt/s6-root \
    && tar -C /opt/s6-root -Jxpf /tmp/s6-overlay-noarch.tar.xz \
    && tar -C /opt/s6-root -Jxpf /tmp/s6-overlay-x86_64.tar.xz \
    && rm -f /tmp/s6-overlay-*.tar.xz

# x264 has no regular release cadence, so use a reviewed immutable commit.
RUN git clone --filter=blob:none https://code.videolan.org/videolan/x264.git /tmp/x264 \
    && cd /tmp/x264 \
    && git checkout "${X264_COMMIT}" \
    && ./configure --prefix=/opt/media --enable-shared --disable-cli \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /tmp/x264

# Build the security-fixed SRT release used by FFmpeg rather than relying on
# the distribution's older library.
RUN git clone --filter=blob:none https://github.com/Haivision/srt.git /tmp/srt \
    && cd /tmp/srt \
    && git checkout "${SRT_COMMIT}" \
    && cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/media \
        -DENABLE_APPS=OFF \
        -DENABLE_SHARED=ON \
        -DENABLE_STATIC=OFF \
    && cmake --build build --parallel \
    && cmake --install build \
    && rm -rf /tmp/srt

RUN git clone --filter=blob:none https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg \
    && cd /tmp/ffmpeg \
    && git checkout "${FFMPEG_COMMIT}" \
    && PKG_CONFIG_PATH=/opt/media/lib/pkgconfig:/opt/media/lib64/pkgconfig \
       ./configure \
        --prefix=/opt/media \
        --disable-debug \
        --disable-doc \
        --disable-ffplay \
        --disable-static \
        --enable-shared \
        --enable-gpl \
        --enable-libopus \
        --enable-libsrt \
        --enable-libx264 \
        --enable-libzimg \
        --extra-cflags=-I/opt/media/include \
        --extra-ldflags=-L/opt/media/lib \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /tmp/ffmpeg

RUN git clone --filter=blob:none https://github.com/cisco/libsrtp.git /tmp/libsrtp \
    && cd /tmp/libsrtp \
    && git checkout "${LIBSRTP_COMMIT}" \
    && cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/janus-deps \
        -DBUILD_SHARED_LIBS=ON \
        -DENABLE_OPENSSL=ON \
        -DENABLE_WARNINGS_AS_ERRORS=OFF \
        -DLIBSRTP_TEST_APPS=OFF \
    && cmake --build build --parallel \
    && cmake --install build \
    && mkdir -p /opt/janus-deps/lib/pkgconfig \
    && printf '%s\n' \
        'prefix=/opt/janus-deps' \
        'exec_prefix=${prefix}' \
        'libdir=${prefix}/lib' \
        'includedir=${prefix}/include' \
        '' \
        'Name: libsrtp2' \
        'Description: Secure RTP library' \
        'Version: 2.8.0' \
        'Libs: -L${libdir} -lsrtp2' \
        'Libs.private: -lcrypto' \
        'Cflags: -I${includedir}' \
        >/opt/janus-deps/lib/pkgconfig/libsrtp2.pc \
    && rm -rf /tmp/libsrtp

COPY patches/janus-streaming-udp-buffer.patch /tmp/janus-streaming-udp-buffer.patch
RUN git clone --filter=blob:none https://github.com/meetecho/janus-gateway.git /tmp/janus \
    && cd /tmp/janus \
    && git checkout "${JANUS_COMMIT}" \
    && git apply /tmp/janus-streaming-udp-buffer.patch \
    && sh autogen.sh \
    && PKG_CONFIG_PATH=/opt/janus-deps/lib/pkgconfig \
       CPPFLAGS=-I/opt/janus-deps/include \
       LDFLAGS=-L/opt/janus-deps/lib \
       ./configure \
        --prefix=/opt/janus \
        --disable-data-channels \
        --disable-mqtt \
        --disable-nanomsg \
        --disable-rabbitmq \
        --disable-unix-sockets \
        --disable-plugin-audiobridge \
        --disable-plugin-echotest \
        --disable-plugin-nosip \
        --disable-plugin-recordplay \
        --disable-plugin-sip \
        --disable-plugin-textroom \
        --disable-plugin-videocall \
        --disable-plugin-videoroom \
        --disable-plugin-voicemail \
        --enable-plugin-streaming \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /tmp/janus /tmp/janus-streaming-udp-buffer.patch

RUN curl -fsSLo /tmp/nginx.tar.gz "https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" \
    && echo "${NGINX_SHA256}  /tmp/nginx.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/nginx.tar.gz -C /tmp \
    && cd "/tmp/nginx-${NGINX_VERSION}" \
    && ./configure \
        --prefix=/opt/nginx \
        --sbin-path=/opt/nginx/sbin/nginx \
        --conf-path=/etc/nginx/nginx.conf \
        --error-log-path=stderr \
        --http-log-path=/dev/stdout \
        --pid-path=/run/webrtc-player/nginx.pid \
        --lock-path=/run/webrtc-player/nginx.lock \
        --with-http_ssl_module \
        --with-http_v2_module \
        --with-http_v3_module \
        --with-threads \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /tmp/nginx*

COPY native/input-selector.c /tmp/input-selector.c
COPY native/config-api.c /tmp/config-api.c
COPY native/janus-monitor.c /tmp/janus-monitor.c
COPY native/network-info.c /tmp/network-info.c
RUN cc -O2 -pipe -Wall -Wextra -Werror -std=c11 -pthread \
        /tmp/input-selector.c -o /opt/input-selector \
    && cc -O2 -pipe -Wall -Wextra -Werror -std=c11 \
        /tmp/config-api.c -o /opt/config-api \
    && cc -O2 -pipe -Wall -Wextra -Werror -std=c11 \
        /tmp/network-info.c -o /opt/network-info \
    && cc -O2 -pipe -Wall -Wextra -Werror -std=c11 \
        /tmp/janus-monitor.c -o /opt/janus-monitor \
        $(pkg-config --cflags --libs libcurl jansson) \
    && strip /opt/input-selector /opt/config-api /opt/network-info /opt/janus-monitor \
    && rm -f /tmp/input-selector.c /tmp/config-api.c /tmp/network-info.c /tmp/janus-monitor.c

# Collect only the shared libraries required by runtime binaries and plugins.
# They are flattened into a private directory to avoid merged-/usr symlink
# collisions when copied into the clean runtime stage.
RUN mkdir -p /opt/runtime-libs \
    && { \
        lddtree -l /opt/media/bin/ffmpeg; \
        lddtree -l /opt/janus/bin/janus; \
        lddtree -l /opt/nginx/sbin/nginx; \
        find /opt/media/lib /opt/media/lib64 -type f -name '*.so*' -exec lddtree -l {} \; 2>/dev/null; \
        find /opt/janus/lib/janus -name '*.so*' -exec lddtree -l {} \;; \
    } | awk '/^\//' | sort -u | xargs -r -I '{}' cp -L '{}' /opt/runtime-libs/ \
    && rm -rf /opt/media/include /opt/media/lib/pkgconfig /opt/media/lib64/pkgconfig \
              /opt/media/share /opt/janus/include /opt/janus/lib/pkgconfig /opt/janus/share

FROM ${UBUNTU_IMAGE} AS runtime

LABEL org.opencontainers.image.title="WebRTC Player" \
      org.opencontainers.image.description="SRT/RTP ingest to WebRTC player" \
      org.opencontainers.image.base.name="ubuntu:26.04" \
      org.opencontainers.image.version="2026.08"

COPY --from=builder /opt/s6-root/ /
COPY --from=builder /opt/runtime-libs/ /opt/runtime-libs/
COPY --from=builder /opt/media/ /opt/media/
COPY --from=builder /opt/janus-deps/ /opt/janus-deps/
COPY --from=builder /opt/janus/ /opt/janus/
COPY --from=builder /opt/nginx/ /opt/nginx/
COPY --from=builder /opt/input-selector /usr/local/bin/input-selector
COPY --from=builder /opt/config-api /usr/local/bin/config-api
COPY --from=builder /opt/network-info /usr/local/bin/network-info
COPY --from=builder /opt/janus-monitor /usr/local/bin/janus-monitor

RUN printf 'player:x:10001:\n' >>/etc/group \
    && printf 'player:x:10001:10001:WebRTC Player:/nonexistent:/usr/sbin/nologin\n' >>/etc/passwd \
    && install -d -o 10001 -g 10001 -m 0750 /config /config/settings

COPY janus/ /etc/webrtc-player/janus/
COPY nginx/nginx.conf /etc/webrtc-player/nginx/nginx.conf
COPY web/ /var/www/html/
COPY entrypoint.sh /etc/cont-init.d/10-configure
COPY scripts/ /usr/local/bin/
COPY services/ /etc/services.d/

RUN chmod 0755 \
        /etc/cont-init.d/10-configure \
        /usr/local/bin/*.sh \
        /etc/services.d/*/run \
        /etc/services.d/*/finish \
    && rm -rf /etc/services.d/input-selector /etc/services.d/srt-relay \
    && for channel in 1 2 3 4 5; do \
        mkdir -p "/etc/services.d/input-selector-${channel}" "/etc/services.d/srt-relay-${channel}"; \
        ln -s /usr/local/bin/run-channel-selector.sh "/etc/services.d/input-selector-${channel}/run"; \
        ln -s /usr/local/bin/finish-channel-service.sh "/etc/services.d/input-selector-${channel}/finish"; \
        ln -s /usr/local/bin/run-channel-relay.sh "/etc/services.d/srt-relay-${channel}/run"; \
        ln -s /usr/local/bin/finish-channel-service.sh "/etc/services.d/srt-relay-${channel}/finish"; \
    done \
    && chown -R player:player /var/www/html

ENV INPUT_MODE=auto \
    NETWORK_MODE=bridge \
    MANAGEMENT_INTERFACE=auto \
    INGEST_INTERFACE=auto \
    WEBRTC_INTERFACE=auto \
    STREAM_ID=1 \
    VIDEO_PORT=5004 \
    AUDIO_PORT=5005 \
    VIDEO_RTCP_PORT=5006 \
    AUDIO_RTCP_PORT=5007 \
    RTP_PACING_US=100 \
    SRT_PUBLIC_PORT=9000 \
    SRT_RELAY_VIDEO_PORT=15004 \
    SRT_RELAY_AUDIO_PORT=15005 \
    SRT_RELAY_VIDEO_RTCP_PORT=15006 \
    SRT_RELAY_AUDIO_RTCP_PORT=15007 \
    JANUS_VIDEO_PORT=25004 \
    JANUS_AUDIO_PORT=25005 \
    JANUS_VIDEO_RTCP_PORT=25006 \
    JANUS_AUDIO_RTCP_PORT=25007 \
    AUDIO_ENABLED=true \
    SRT_URL=srt://0.0.0.0:9000?mode=listener\&latency=120000 \
    SRT_AUDIO=true \
    SRT_COLOR_MODE=auto \
    VIDEO_BITRATE=6M \
    VIDEO_BUFFER_SIZE=2M \
    VIDEO_PRESET=veryfast \
    AUDIO_BITRATE=128k \
    MAX_FPS=60 \
    LD_LIBRARY_PATH=/opt/media/lib:/opt/media/lib64:/opt/janus-deps/lib:/opt/runtime-libs \
    S6_BEHAVIOUR_IF_STAGE2_FAILS=2 \
    S6_CMD_WAIT_FOR_SERVICES_MAXTIME=30000

EXPOSE 8088/tcp 5004-5023/udp 9000-9004/udp 20000-20100/udp

HEALTHCHECK --interval=15s --timeout=5s --start-period=20s --retries=3 \
    CMD ["/usr/local/bin/healthcheck.sh"]

ENTRYPOINT ["/init"]

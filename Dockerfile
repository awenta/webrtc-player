FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    nginx \
    ffmpeg \
    libmicrohttpd-dev libjansson-dev libssl-dev \
    libsofia-sip-ua-dev libglib2.0-dev libopus-dev \
    libogg-dev libcurl4-openssl-dev liblua5.3-dev \
    libconfig-dev pkg-config gengetopt libtool automake \
    libnice-dev libwebsockets-dev cmake git \
    libsrtp2-dev libusrsctp-dev \
    supervisor curl \
    && rm -rf /var/lib/apt/lists/*

# Build Janus Gateway
RUN cd /tmp && \
    git clone --depth 1 --branch v1.2.4 https://github.com/meetecho/janus-gateway.git && \
    cd janus-gateway && \
    sh autogen.sh && \
    ./configure --prefix=/usr \
      --sysconfdir=/etc \
      --disable-rabbitmq \
      --disable-mqtt \
      --disable-nanomsg \
      --disable-unix-sockets \
      --disable-data-channels \
      --enable-plugin-streaming \
      --disable-plugin-echotest \
      --disable-plugin-recordplay \
      --disable-plugin-sip \
      --disable-plugin-nosip \
      --disable-plugin-videocall \
      --disable-plugin-voicemail \
      --disable-plugin-textroom \
      --disable-plugin-audiobridge \
      --disable-plugin-videoroom \
    && make -j$(nproc) && make install && make configs && \
    rm -rf /tmp/janus-gateway

# Copy Janus config
COPY janus/janus.jcfg /etc/janus/janus.jcfg
COPY janus/janus.plugin.streaming.jcfg /etc/janus/janus.plugin.streaming.jcfg
COPY janus/janus.transport.websockets.jcfg /etc/janus/janus.transport.websockets.jcfg

# Copy web frontend
COPY web/ /var/www/html/

# Copy nginx config
COPY nginx/nginx.conf /etc/nginx/nginx.conf

# Copy entrypoint and supervisor config
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
COPY supervisord.conf /etc/supervisor/conf.d/supervisord.conf

# HTTP
EXPOSE 8080
# Janus RTP range for incoming streams
EXPOSE 20000-20100/udp

ENTRYPOINT ["/entrypoint.sh"]

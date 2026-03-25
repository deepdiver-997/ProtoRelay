FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential \
    cmake \
    pkg-config \
    curl \
    git \
    libssl-dev \
    libcurl4-openssl-dev \
    libboost-all-dev \
    default-libmysqlclient-dev \
    libspdlog-dev \
    libc-ares-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN chmod +x ./build.sh
RUN BUILD_JOBS=4 ./build.sh Release clean

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    libcurl4 \
    libc-ares2 \
    libstdc++6 \
    libmariadb3 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
RUN mkdir -p /app/config /app/certs /app/logs /app/mail /app/attachments

COPY --from=builder /src/test/smtpsServer /app/smtpsServer
COPY --from=builder /usr/lib/*-linux-gnu/libmysqlclient.so.* /usr/local/lib/
COPY --from=builder /usr/lib/*-linux-gnu/libspdlog.so.* /usr/local/lib/
COPY --from=builder /usr/lib/*-linux-gnu/libfmt.so.* /usr/local/lib/
RUN ldconfig

ENTRYPOINT ["/app/smtpsServer"]
CMD ["-c", "/app/config/smtpsConfig_hdfs_web.json"]

FROM debian:trixie-slim AS build-env

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       git \
       libssl-dev \
       libcurl4-openssl-dev \
       libnghttp2-dev \
       libsqlite3-dev \
       make \
       pkg-config \
       zlib1g-dev \
       npm \
       nodejs \
       python3 \
       sqlite3 \
       unzip \
       wget \
       ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY . .

# Force every compilation to use AddressSanitizer so *all* libraries are instrumented
RUN mv /usr/bin/gcc /usr/bin/gcc.real && \
    echo '#!/bin/sh\nexec /usr/bin/gcc.real -fsanitize=address -g -O1 "$@"' > /usr/bin/gcc && \
    chmod +x /usr/bin/gcc
RUN mv /usr/bin/g++ /usr/bin/g++.real && \
    echo '#!/bin/sh\nexec /usr/bin/g++.real -fsanitize=address -g -O1 "$@"' > /usr/bin/g++ && \
    chmod +x /usr/bin/g++

RUN make -C lib/libttak clean
RUN make -C .deps/cwist clean
RUN make CWIST_SYSTEM_SQLITE=1

WORKDIR /app/web
RUN npm install
RUN npm run build

FROM debian:trixie-slim

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       libcurl4t64 \
       libnghttp2-14 \
       libasan8 \
       nodejs \
       sqlite3 \
       gdb \
       procps \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build-env /app /app

ENV NODE_ENV=production
ENV PORT=23345
ENV LOGANA_CONFIG=/etc/logana/collect.ini
ENV LOGANA_ENGINE_PORT=24445

EXPOSE 23345

RUN chmod +x /app/entrypoint.sh

CMD ["/app/entrypoint.sh"]

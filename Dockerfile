FROM debian:trixie-slim AS build-env

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       python3 \
       make \
       nodejs \
       npm \
       ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY . .

RUN make -C lib/libttak clean
RUN make

WORKDIR /app/web
RUN npm install
RUN npm run build

FROM debian:trixie-slim

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build-env /app /app

ENV NODE_ENV=production
ENV PORT=23345
ENV LOGANA_CONFIG=/etc/logana/collect.ini
ENV LOGANA_ENGINE_PORT=24445

EXPOSE 23345

RUN chmod +x /app/entrypoint.sh

CMD ["/app/entrypoint.sh"]

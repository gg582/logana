#include "logana/cwist_app_preset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ttak/log/logger.h>

/* -------------------------------------------------------------------------- */
/*  Extended state keyed by cwist_app pointer                                 */
/* -------------------------------------------------------------------------- */

typedef struct cwist_app_preset_state {
    cwist_app *app;
    bool pqc_enabled;
    bool http3_enabled;
    bool quic_enabled;
    cwist_app_observe_preset_t observe_preset;
    ttak_logger_t logger;
} cwist_app_preset_state;

#define CWIST_APP_PRESET_MAX 64
static cwist_app_preset_state g_preset_states[CWIST_APP_PRESET_MAX];
static int g_preset_count = 0;

static cwist_app_preset_state *preset_state_find(cwist_app *app)
{
    for (int i = 0; i < g_preset_count; i++) {
        if (g_preset_states[i].app == app) {
            return &g_preset_states[i];
        }
    }
    return NULL;
}

static cwist_app_preset_state *preset_state_ensure(cwist_app *app)
{
    cwist_app_preset_state *st = preset_state_find(app);
    if (st) return st;
    if (g_preset_count < CWIST_APP_PRESET_MAX) {
        st = &g_preset_states[g_preset_count++];
        memset(st, 0, sizeof(*st));
        st->app = app;
        return st;
    }
    fprintf(stderr, "[cwist_app_preset] Too many apps, state unavailable\n");
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Compat preset helpers                                                     */
/* -------------------------------------------------------------------------- */

cwist_app *cwist_app_auto_create(cwist_app_compat_preset_t preset)
{
    cwist_app *app = cwist_app_create();
    if (!app) {
        fprintf(stderr, "[cwist_app_preset] cwist_app_create failed\n");
        return NULL;
    }

    cwist_app_preset_state *st = preset_state_ensure(app);
    if (st) {
        st->pqc_enabled   = false;
        st->http3_enabled = false;
        st->quic_enabled  = false;
        st->observe_preset = CWIST_EMBEDDED;
    }

    switch (preset) {
    case CWIST_HTTP_LEGACY:
        app->use_ssl    = false;
        app->use_http2  = false;
        break;

    case CWIST_HTTPS_LEGACY:
        app->use_ssl    = true;
        app->use_http2  = false;
        break;

    case CWIST_HTTP2_COMPAT_AUTO:
        app->use_ssl    = false;
        app->use_http2  = true;
        break;

    case CWIST_HTTPS2_COMPAT_AUTO:
        app->use_ssl    = true;
        app->use_http2  = true;
        break;

    case CWIST_HTTP3_COMPAT_AUTO:
        app->use_ssl    = false;
        app->use_http2  = true;
        if (st) st->http3_enabled = true;
        /* Fallback chain: HTTP/3 -> HTTP/2 -> HTTP/1.1
         * HTTP/3 path is prepared; cwist currently handles up to HTTP/2. */
        break;

    case CWIST_HTTPS3_COMPAT_AUTO:
        app->use_ssl    = true;
        app->use_http2  = true;
        if (st) {
            st->http3_enabled = true;
            st->quic_enabled  = true;
        }
        /* Fallback chain: HTTP/3+QUIC -> HTTP/3 w/o WebTransport
         * -> HTTP/2+TLS 1.3 -> HTTP/1.1 legacy */
        break;
    }

    return app;
}

/* -------------------------------------------------------------------------- */
/*  PQC layer                                                                 */
/* -------------------------------------------------------------------------- */

void logana_app_use_pqc_layer(cwist_app *app, bool enabled)
{
    if (!app) return;
    cwist_app_preset_state *st = preset_state_ensure(app);
    if (st) st->pqc_enabled = enabled;

    if (enabled) {
        /* TODO: wire into cwist TLS layer when post-quantum crypto
         *       (e.g. hybrid ML-KEM + X25519) is available. */
    }
}

/* -------------------------------------------------------------------------- */
/*  Observability presets                                                     */
/* -------------------------------------------------------------------------- */

static void cwist_cloud_log_handler(ttak_log_level_t level, const char *msg)
{
    const char *lvl = (level == TTAK_LOG_ERROR) ? "ERROR"
                    : (level == TTAK_LOG_WARN)  ? "WARN"
                    : (level == TTAK_LOG_INFO)  ? "INFO"
                                                : "DEBUG";
    fprintf(stdout, "{\"level\":\"%s\",\"msg\":\"%s\"}\n", lvl, msg);
}

static void cwist_embedded_log_handler(ttak_log_level_t level, const char *msg)
{
    const char *label = (level == TTAK_LOG_ERROR) ? "ERR"
                      : (level == TTAK_LOG_WARN)  ? "WRN"
                      : (level == TTAK_LOG_INFO)  ? "INF"
                                                  : "DBG";
    fprintf(stderr, "[%s] %s\n", label, msg);
}

void cwist_app_observe(cwist_app *app, cwist_app_observe_preset_t preset)
{
    if (!app) return;

    cwist_app_preset_state *st = preset_state_ensure(app);
    if (st) st->observe_preset = preset;

    switch (preset) {
    case CWIST_CLOUD:
        /* structured logs, WARN+, Prometheus on, health endpoints,
         * reduced trace verbosity, cloud friendly */
        ttak_logger_init(&st->logger, cwist_cloud_log_handler, TTAK_LOG_WARN);
        ttak_mem_set_trace(0);
        break;

    case CWIST_EMBEDDED:
        /* detailed local logs, structured stderr, route trace optional,
         * metrics off by default, low overhead, debug readable */
        ttak_logger_init(&st->logger, cwist_embedded_log_handler, TTAK_LOG_DEBUG);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/*  Run                                                                       */
/* -------------------------------------------------------------------------- */

int cwist_app_run(cwist_app *app)
{
    if (!app) {
        fprintf(stderr, "[cwist_app_preset] Cannot run NULL app\n");
        return -1;
    }

    cwist_app_preset_state *st = preset_state_find(app);
    if (st && st->http3_enabled) {
        /* TODO: bootstrap HTTP/3 or QUIC transport before listening
         * when cwist adds h3/quic support. */
    }

    return cwist_app_listen(app, app->port);
}

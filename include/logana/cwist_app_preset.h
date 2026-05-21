#ifndef LOGANA_CWIST_APP_PRESET_H
#define LOGANA_CWIST_APP_PRESET_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>

/**
 * @brief Transport compatibility presets.
 *
 * Each preset configures the fallback chain automatically.
 */
typedef enum {
    CWIST_HTTP2_COMPAT_AUTO,   /**< HTTP/2 preferred, fallback to HTTP/1.1 */
    CWIST_HTTPS2_COMPAT_AUTO,  /**< HTTP/2+TLS 1.3 preferred, fallback to legacy HTTPS/1.1 */
    CWIST_HTTP3_COMPAT_AUTO,   /**< HTTP/3 -> HTTP/2 -> HTTP/1.1 */
    CWIST_HTTPS3_COMPAT_AUTO,  /**< HTTP/3+QUIC -> HTTP/3 w/o WT -> HTTP/2+TLS 1.3 -> HTTP/1.1 legacy */
    CWIST_HTTPS_LEGACY,        /**< HTTP/1.1 over TLS only */
    CWIST_HTTP_LEGACY          /**< Plain HTTP/1.1 only */
} cwist_app_compat_preset_t;

/**
 * @brief Observability target presets.
 */
typedef enum {
    CWIST_CLOUD,    /**< Structured logs, WARN+, Prometheus, health endpoints, reduced trace verbosity */
    CWIST_EMBEDDED  /**< Detailed local logs, structured stderr, optional route trace, low overhead */
} cwist_app_observe_preset_t;

/**
 * @brief Create a cwist_app with a compatibility preset applied.
 * @param preset Desired transport fallback chain.
 * @return New application instance, or NULL on failure.
 */
cwist_app *cwist_app_auto_create(cwist_app_compat_preset_t preset);

/**
 * @brief Enable or disable the post-quantum cryptography layer.
 * @param app Target application.
 * @param enabled true to request PQC handshakes when available.
 */
void cwist_app_use_pqc_layer(cwist_app *app, bool enabled);

/**
 * @brief Apply an observability preset.
 * @param app Target application.
 * @param preset CWIST_CLOUD or CWIST_EMBEDDED.
 */
void cwist_app_observe(cwist_app *app, cwist_app_observe_preset_t preset);

/**
 * @brief Run the application.
 *
 * Wraps the underlying listen loop and applies any preset-specific
 * bootstrap (e.g. HTTP/3, WebTransport, Prometheus endpoint).
 *
 * @param app Application to run.
 * @return 0 on graceful shutdown, non-zero on error.
 */
int cwist_app_run(cwist_app *app);

#endif

/**
 * nff_port_esp32_arduino.c — Arduino core for ESP32 platform implementation.
 *
 * Uses: PubSubClient + WiFiClientSecure, Preferences (NVS), Update.h + HTTPClient,
 * mbedTLS ECDSA (linked by ESP32 Arduino core), FreeRTOS for task/mutex.
 *
 * Requires the PubSubClient library installed in the Arduino Library Manager.
 */

#if defined(ARDUINO) && defined(ESP32)

#include "nff_port.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

uint32_t nff_port_millis(void) { return (uint32_t)millis(); }
void     nff_port_delay_ms(uint32_t ms) { delay(ms); }

/* ------------------------------------------------------------------ */
/* Mutex                                                                */
/* ------------------------------------------------------------------ */

nff_mutex_t nff_port_mutex_create(void) {
    return (nff_mutex_t)xSemaphoreCreateMutex();
}
void nff_port_mutex_lock(nff_mutex_t m)    { xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY); }
void nff_port_mutex_unlock(nff_mutex_t m)  { xSemaphoreGive((SemaphoreHandle_t)m); }
void nff_port_mutex_destroy(nff_mutex_t m) { vSemaphoreDelete((SemaphoreHandle_t)m); }

/* ------------------------------------------------------------------ */
/* Task                                                                 */
/* ------------------------------------------------------------------ */

void nff_port_task_create(void (*fn)(void *), const char *name,
                           uint32_t stack_bytes, void *arg,
                           uint8_t priority, int core) {
    int c = (core >= 0 && core <= 1) ? core : tskNO_AFFINITY;
    xTaskCreatePinnedToCore((TaskFunction_t)fn, name,
                             stack_bytes / sizeof(StackType_t),
                             arg, priority, NULL, c);
}

/* ------------------------------------------------------------------ */
/* MQTT (PubSubClient + WiFiClientSecure)                              */
/* ------------------------------------------------------------------ */

struct nff_mqtt_handle {
    WiFiClientSecure  tls;
    PubSubClient      client;
    char              host[256];
    uint16_t          port;
    char              client_id[64];
    char              lwt_topic[NFF_TOPIC_MAXLEN];
    char              lwt_payload[256];
    bool              tls_configured;
    void (*rx_cb)(const char *, const uint8_t *, size_t, void *);
    void *rx_user;

    nff_mqtt_handle() : client(tls), tls_configured(false), rx_cb(nullptr), rx_user(nullptr) {}
};

static void pubsub_callback(char *topic, byte *payload, unsigned int length) {
    /* PubSubClient callback is registered per-handle via a static dispatch table */
    /* We use a single global handle pointer since Arduino sketches have one MQTT connection */
    extern struct nff_mqtt_handle *g_nff_mqtt_handle;
    if (g_nff_mqtt_handle && g_nff_mqtt_handle->rx_cb) {
        g_nff_mqtt_handle->rx_cb(topic, (const uint8_t *)payload, (size_t)length,
                                  g_nff_mqtt_handle->rx_user);
    }
}

struct nff_mqtt_handle *g_nff_mqtt_handle = nullptr;

nff_mqtt_handle_t *nff_port_mqtt_create(void) {
    struct nff_mqtt_handle *h = new struct nff_mqtt_handle();
    g_nff_mqtt_handle = h;
    h->client.setCallback(pubsub_callback);
    return (nff_mqtt_handle_t *)h;
}

void nff_port_mqtt_set_server(nff_mqtt_handle_t *hh,
                               const char *host, uint16_t port) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    strncpy(h->host, host, sizeof(h->host) - 1);
    h->port = port;
    h->client.setServer(host, port);
}

void nff_port_mqtt_set_tls(nff_mqtt_handle_t *hh,
                            const uint8_t *ca, size_t ca_len,
                            const uint8_t *cert, size_t cert_len,
                            const uint8_t *key, size_t key_len) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    h->tls.setCACert((const char *)ca);         (void)ca_len;
    h->tls.setCertificate((const char *)cert);  (void)cert_len;
    h->tls.setPrivateKey((const char *)key);    (void)key_len;
    h->tls_configured = true;
}

void nff_port_mqtt_set_rx_callback(nff_mqtt_handle_t *hh,
                                    void (*cb)(const char *, const uint8_t *,
                                               size_t, void *),
                                    void *user) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    h->rx_cb   = cb;
    h->rx_user = user;
}

int nff_port_mqtt_connect(nff_mqtt_handle_t *hh,
                           const char *client_id,
                           const char *lwt_topic,
                           const char *lwt_payload) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    if (client_id)   strncpy(h->client_id,  client_id,  sizeof(h->client_id)  - 1);
    if (lwt_topic)   strncpy(h->lwt_topic,   lwt_topic,  sizeof(h->lwt_topic)  - 1);
    if (lwt_payload) strncpy(h->lwt_payload, lwt_payload, sizeof(h->lwt_payload) - 1);

    if (!h->tls_configured) h->tls.setInsecure(); /* dev only — no cert check */

    bool ok = h->client.connect(h->client_id,
                                 "", "",  /* no username/password — mTLS is auth */
                                 h->lwt_topic, 1, true, h->lwt_payload);
    return ok ? 0 : -1;
}

int nff_port_mqtt_subscribe(nff_mqtt_handle_t *hh, const char *topic, int qos) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    return h->client.subscribe(topic, qos) ? 0 : -1;
}

int nff_port_mqtt_publish(nff_mqtt_handle_t *hh, const char *topic,
                           const char *payload, int qos, bool retain) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    return h->client.publish(topic, payload, retain) ? 0 : -1;
    (void)qos;
}

void nff_port_mqtt_loop(nff_mqtt_handle_t *hh) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    h->client.loop();
}

bool nff_port_mqtt_is_connected(nff_mqtt_handle_t *hh) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)hh;
    return h->client.connected();
}

/* ------------------------------------------------------------------ */
/* NVS (Preferences)                                                    */
/* ------------------------------------------------------------------ */

static Preferences s_prefs;
static bool        s_prefs_open = false;

static void prefs_ensure(void) {
    if (!s_prefs_open) {
        s_prefs.begin("nff", false);
        s_prefs_open = true;
    }
}

int nff_port_nvs_set_str(const char *key, const char *value) {
    prefs_ensure();
    return s_prefs.putString(key, value) > 0 ? 0 : -1;
}

int nff_port_nvs_get_str(const char *key, char *out, size_t out_len) {
    prefs_ensure();
    String v = s_prefs.getString(key, "");
    if (v.length() == 0) return -1;
    strncpy(out, v.c_str(), out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

int nff_port_nvs_set_u32(const char *key, uint32_t value) {
    prefs_ensure();
    return s_prefs.putUInt(key, value) ? 0 : -1;
}

int nff_port_nvs_get_u32(const char *key, uint32_t *out) {
    prefs_ensure();
    if (!s_prefs.isKey(key)) return -1;
    *out = s_prefs.getUInt(key, 0);
    return 0;
}

int nff_port_nvs_erase_key(const char *key) {
    prefs_ensure();
    s_prefs.remove(key);
    return 0;
}

int nff_port_nvs_commit(void) {
    return 0; /* Preferences auto-commits */
}

/* ------------------------------------------------------------------ */
/* OTA (Update.h + HTTPClient)                                         */
/* ------------------------------------------------------------------ */

struct ota_ctx { bool active; };
static struct ota_ctx s_ota_ctx;

nff_ota_handle_t nff_port_ota_begin(size_t expected_size) {
    if (!Update.begin(expected_size > 0 ? expected_size : UPDATE_SIZE_UNKNOWN)) {
        return NFF_OTA_INVALID_HANDLE;
    }
    s_ota_ctx.active = true;
    return (nff_ota_handle_t)&s_ota_ctx;
}

int nff_port_ota_write(nff_ota_handle_t h, const uint8_t *buf, size_t len) {
    (void)h;
    return (Update.write((uint8_t *)buf, len) == len) ? 0 : -1;
}

int nff_port_ota_end(nff_ota_handle_t h) {
    (void)h;
    s_ota_ctx.active = false;
    return Update.end(true) ? 0 : -1;
}

void nff_port_ota_abort(nff_ota_handle_t h) {
    (void)h;
    Update.abort();
    s_ota_ctx.active = false;
}

void nff_port_reboot(void) { ESP.restart(); }

/* ------------------------------------------------------------------ */
/* HTTPS streaming                                                      */
/* ------------------------------------------------------------------ */

int nff_port_https_get_stream(const char *url,
                               int (*chunk_cb)(const uint8_t *, size_t, void *),
                               void *user_ctx, uint32_t timeout_ms) {
    WiFiClientSecure tls;
    tls.setInsecure(); /* Production: use pinned CA cert */
    HTTPClient http;
    http.begin(tls, url);
    http.setTimeout((int)timeout_ms);
    int http_code = http.GET();
    if (http_code != 200) { http.end(); return -1; }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t   buf[512];
    int       rc = 0;
    int       available;

    while (http.connected() && (available = stream->available()) >= 0) {
        int n = stream->readBytes(buf, sizeof(buf));
        if (n <= 0) break;
        rc = chunk_cb(buf, (size_t)n, user_ctx);
        if (rc < 0) break;
    }
    http.end();
    return rc;
}

/* ------------------------------------------------------------------ */
/* SHA-256                                                              */
/* ------------------------------------------------------------------ */

nff_sha256_ctx_t nff_port_sha256_new(void) {
    mbedtls_sha256_context *c = (mbedtls_sha256_context *)malloc(sizeof(*c));
    mbedtls_sha256_init(c);
    mbedtls_sha256_starts(c, 0);
    return (nff_sha256_ctx_t)c;
}
void nff_port_sha256_update(nff_sha256_ctx_t c, const uint8_t *d, size_t l) {
    mbedtls_sha256_update((mbedtls_sha256_context *)c, d, l);
}
void nff_port_sha256_finish(nff_sha256_ctx_t c, uint8_t out[32]) {
    mbedtls_sha256_finish((mbedtls_sha256_context *)c, out);
}
void nff_port_sha256_free(nff_sha256_ctx_t c) {
    mbedtls_sha256_free((mbedtls_sha256_context *)c);
    free(c);
}

/* ------------------------------------------------------------------ */
/* ECDSA-P256 verify (mbedTLS — same as IDF port)                      */
/* ------------------------------------------------------------------ */

int nff_port_ecdsa_p256_verify(const uint8_t *pub_key_65,
                                const uint8_t *msg,    size_t msg_len,
                                const uint8_t *sig_der, size_t sig_len) {
    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    int rc = -1;

    if (mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp),
                                MBEDTLS_ECP_DP_SECP256R1) != 0) goto out;
    if (mbedtls_ecp_point_read_binary(&ctx.MBEDTLS_PRIVATE(grp),
                                       &ctx.MBEDTLS_PRIVATE(Q),
                                       pub_key_65, 65) != 0) goto out;

    uint8_t digest[32];
    mbedtls_sha256(msg, msg_len, digest, 0);
    rc = (mbedtls_ecdsa_read_signature(&ctx, digest, 32, sig_der, sig_len) == 0)
         ? 0 : -1;
out:
    mbedtls_ecdsa_free(&ctx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* System diagnostics                                                   */
/* ------------------------------------------------------------------ */

#include <esp_heap_caps.h>
#include <WiFi.h>

void nff_port_get_diag_info(nff_diag_info_t *out) {
    out->free_heap     = (uint32_t)esp_get_free_heap_size();
    out->min_free_heap = (uint32_t)esp_get_minimum_free_heap_size();
    out->uptime_ms     = nff_port_millis();
    out->wifi_rssi     = (int32_t)WiFi.RSSI();
    out->cpu_count     = 2;
}

/* ------------------------------------------------------------------ */
/* Panic hook                                                           */
/* ------------------------------------------------------------------ */

static void (*s_panic_hook)(const char *) = nullptr;

static void IRAM_ATTR nff_arduino_panic(const esp_panic_info_t *info) {
    if (s_panic_hook) {
        char r[64];
        snprintf(r, sizeof(r), "cause=%d pc=0x%08x",
                 (int)info->exception_cause,
                 (unsigned)(uintptr_t)info->exception_addr);
        s_panic_hook(r);
    }
    esp_default_panic_handler(info);
}

void nff_port_install_panic_hook(void (*fn)(const char *)) {
    s_panic_hook = fn;
    esp_set_panic_handler(nff_arduino_panic);
}

/* ------------------------------------------------------------------ */
/* AMP                                                                  */
/* ------------------------------------------------------------------ */

int nff_port_stall_core(int id)   { esp_cpu_stall(id);   return 0; }
int nff_port_unstall_core(int id) { esp_cpu_unstall(id); return 0; }

#endif /* ARDUINO && ESP32 */

/**
 * nff_port_esp32_arduino.c — Arduino core for ESP32 platform implementation.
 *
 * Uses: PubSubClient + WiFiClientSecure, Preferences (NVS), Update.h + HTTPClient,
 * mbedTLS ECDSA (linked by ESP32 Arduino core), FreeRTOS for task/mutex.
 *
 * Requires the PubSubClient library installed in the Arduino Library Manager.
 */

#if defined(ARDUINO) && defined(ESP32)

#include "nff.h"
#include "nff_port.h"
#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
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

/* Convert DER bytes to a null-terminated PEM string (heap-allocated). */
static char *der_to_pem(const char *header, const char *footer,
                         const uint8_t *der, size_t der_len) {
    size_t b64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, der, der_len);

    uint8_t *b64 = (uint8_t *)malloc(b64_len + 1);
    if (!b64) return nullptr;
    size_t actual = 0;
    mbedtls_base64_encode(b64, b64_len + 1, &actual, der, der_len);

    size_t hdr = strlen(header), ftr = strlen(footer);
    size_t lines = (actual + 63) / 64;
    char *pem = (char *)malloc(hdr + 1 + actual + lines + ftr + 2);
    if (!pem) { free(b64); return nullptr; }

    char *p = pem;
    memcpy(p, header, hdr); p += hdr; *p++ = '\n';
    for (size_t pos = 0; pos < actual; ) {
        size_t n = (actual - pos > 64) ? 64 : (actual - pos);
        memcpy(p, b64 + pos, n); p += n; *p++ = '\n'; pos += n;
    }
    memcpy(p, footer, ftr); p += ftr; *p++ = '\n'; *p = '\0';
    free(b64);
    return pem;
}

struct nff_mqtt_handle {
    WiFiClientSecure  tls;
    PubSubClient      client;
    char              host[256];
    uint16_t          port;
    char              client_id[64];
    char              lwt_topic[NFF_TOPIC_MAXLEN];
    char              lwt_payload[256];
    bool              tls_configured;
    char             *ca_pem;
    char             *cert_pem;
    char             *key_pem;
    void (*rx_cb)(const char *, const uint8_t *, size_t, void *);
    void *rx_user;

    nff_mqtt_handle() : client(tls), tls_configured(false),
                        ca_pem(nullptr), cert_pem(nullptr), key_pem(nullptr),
                        rx_cb(nullptr), rx_user(nullptr) {}
    ~nff_mqtt_handle() { free(ca_pem); free(cert_pem); free(key_pem); }
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
    /* PubSubClient defaults to a 256B buffer, which silently drops larger
       inbound PUBLISHes (e.g. ~400B+ OTA commands). Enlarge once at create. */
    h->client.setBufferSize(NFF_MQTT_BUFFER_SIZE);
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
    free(h->ca_pem);
    free(h->cert_pem);
    free(h->key_pem);
    h->ca_pem   = der_to_pem("-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----", ca,   ca_len);
    h->cert_pem = der_to_pem("-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----", cert, cert_len);
    h->key_pem  = der_to_pem("-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----",  key,  key_len);
    if (h->ca_pem)   h->tls.setCACert(h->ca_pem);
    if (h->cert_pem) h->tls.setCertificate(h->cert_pem);
    if (h->key_pem)  h->tls.setPrivateKey(h->key_pem);
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

    int        total  = http.getSize();        /* Content-Length, or -1 if unknown */
    WiFiClient *stream = http.getStreamPtr();
    uint8_t    buf[1024];
    int        rc = 0;
    size_t     received = 0;
    uint32_t   last_data = millis();

    /* Read until we have the full Content-Length (or, if unknown, until the
       peer disconnects). Do NOT break on a transient empty read — a multi-
       hundred-KB TLS transfer routinely stalls >1s between records, and the
       old `readBytes()<=0 -> break` truncated the image and still returned
       success, causing a SHA-256 mismatch on a partial download. */
    while (true) {
        if (total >= 0 && received >= (size_t)total) break;   /* got it all */

        size_t avail = stream->available();
        if (avail > 0) {
            size_t want = avail > sizeof(buf) ? sizeof(buf) : avail;
            int n = stream->readBytes(buf, want);
            if (n > 0) {
                rc = chunk_cb(buf, (size_t)n, user_ctx);
                if (rc < 0) break;
                received += (size_t)n;
                last_data = millis();
            }
        } else {
            if (!http.connected() && stream->available() == 0) break;  /* EOF */
            if (millis() - last_data > timeout_ms) { rc = -1; break; }  /* real stall */
            delay(1);
        }
    }
    http.end();

    /* A short read with no explicit error is still a failure — never let a
       truncated image reach the SHA-256 check as if it succeeded. */
    if (rc == 0 && total >= 0 && received != (size_t)total) rc = -1;
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

void nff_port_get_hw_info(nff_hw_info_t *out) {
    const char *model = ESP.getChipModel();   /* e.g. "ESP32-D0WD-V3", "ESP32-S3" */
    snprintf(out->chip_model, sizeof(out->chip_model), "%s", model ? model : "");
    out->revision   = (uint8_t)ESP.getChipRevision();
    out->flash_size = (uint32_t)ESP.getFlashChipSize();
    out->cores      = (uint8_t)ESP.getChipCores();

    /* Derive the canonical device_type family from the model string. The variant
     * letters (S3/S2/C3/C6/H2) disambiguate; a plain ESP32 has none. */
    const char *t = "";
    if      (strstr(model ? model : "", "S3")) t = "esp32s3";
    else if (strstr(model ? model : "", "S2")) t = "esp32s2";
    else if (strstr(model ? model : "", "C3")) t = "esp32c3";
    else if (strstr(model ? model : "", "C6")) t = "esp32c6";
    else if (strstr(model ? model : "", "H2")) t = "esp32h2";
    else if (strstr(model ? model : "", "ESP32")) t = "esp32";
    snprintf(out->device_type, sizeof(out->device_type), "%s", t);
}

/* ------------------------------------------------------------------ */
/* Panic hook                                                           */
/* ------------------------------------------------------------------ */

static void (*s_panic_hook)(const char *) = nullptr;

void nff_port_install_panic_hook(void (*fn)(const char *)) {
    /* esp_set_panic_handler is not exposed in arduino-esp32 3.x;
     * crash detection still works via esp_reset_reason() on next boot. */
    s_panic_hook = fn;
}

/* ------------------------------------------------------------------ */
/* AMP                                                                  */
/* ------------------------------------------------------------------ */

int nff_port_stall_core(int id)   { esp_cpu_stall(id);   return 0; }
int nff_port_unstall_core(int id) { esp_cpu_unstall(id); return 0; }

#endif /* ARDUINO && ESP32 */

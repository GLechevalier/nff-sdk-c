/**
 * nff_port_esp32_idf.c — ESP-IDF platform implementation.
 *
 * Covers: FreeRTOS mutex/task, esp-mqtt event-driven MQTT, NVS, esp_https_ota,
 * mbedTLS ECDSA, esp_cpu_stall (dual-core AMP), esp_reset_reason (crash detect).
 */

#if defined(ESP_PLATFORM) && !defined(ARDUINO)

#include "nff_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "nff_port";

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

uint32_t nff_port_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void nff_port_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ------------------------------------------------------------------ */
/* Mutex                                                                */
/* ------------------------------------------------------------------ */

nff_mutex_t nff_port_mutex_create(void) {
    return (nff_mutex_t)xSemaphoreCreateMutex();
}

void nff_port_mutex_lock(nff_mutex_t m) {
    xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY);
}

void nff_port_mutex_unlock(nff_mutex_t m) {
    xSemaphoreGive((SemaphoreHandle_t)m);
}

void nff_port_mutex_destroy(nff_mutex_t m) {
    vSemaphoreDelete((SemaphoreHandle_t)m);
}

/* ------------------------------------------------------------------ */
/* Task                                                                 */
/* ------------------------------------------------------------------ */

void nff_port_task_create(void (*fn)(void *), const char *name,
                           uint32_t stack_bytes, void *arg,
                           uint8_t priority, int core) {
    int c = (core == 0) ? PRO_CPU_NUM : (core == 1) ? APP_CPU_NUM : tskNO_AFFINITY;
    xTaskCreatePinnedToCore((TaskFunction_t)fn, name,
                             stack_bytes / sizeof(StackType_t),
                             arg, priority, NULL, c);
}

/* ------------------------------------------------------------------ */
/* MQTT (esp-mqtt event-driven)                                        */
/* ------------------------------------------------------------------ */

struct nff_mqtt_handle {
    esp_mqtt_client_handle_t client;
    bool                     connected;
    const uint8_t           *ca;    size_t ca_len;
    const uint8_t           *cert;  size_t cert_len;
    const uint8_t           *key;   size_t key_len;
    char                     host[256];
    uint16_t                 port;
    char                     lwt_topic[128];
    char                     lwt_payload[256];
    void (*rx_cb)(const char *, const uint8_t *, size_t, void *);
    void *rx_user;
};

static void mqtt_event_handler(void *handler_arg, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    struct nff_mqtt_handle *h = (struct nff_mqtt_handle *)handler_arg;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        h->connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        h->connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (h->rx_cb) {
            /* ev->topic is NOT null-terminated; copy */
            char topic[NFF_TOPIC_MAXLEN];
            int tlen = ev->topic_len < (int)sizeof(topic) - 1
                       ? ev->topic_len : (int)sizeof(topic) - 1;
            memcpy(topic, ev->topic, (size_t)tlen);
            topic[tlen] = '\0';
            h->rx_cb(topic, (const uint8_t *)ev->data,
                     (size_t)ev->data_len, h->rx_user);
        }
        break;
    default:
        break;
    }
}

nff_mqtt_handle_t *nff_port_mqtt_create(void) {
    struct nff_mqtt_handle *h = calloc(1, sizeof(*h));
    return (nff_mqtt_handle_t *)h;
}

void nff_port_mqtt_set_server(nff_mqtt_handle_t *h,
                               const char *host, uint16_t port) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    strncpy(mh->host, host, sizeof(mh->host) - 1);
    mh->port = port;
}

void nff_port_mqtt_set_tls(nff_mqtt_handle_t *h,
                            const uint8_t *ca, size_t ca_len,
                            const uint8_t *cert, size_t cert_len,
                            const uint8_t *key, size_t key_len) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    mh->ca       = ca;   mh->ca_len   = ca_len;
    mh->cert     = cert; mh->cert_len = cert_len;
    mh->key      = key;  mh->key_len  = key_len;
}

void nff_port_mqtt_set_rx_callback(nff_mqtt_handle_t *h,
                                    void (*cb)(const char *, const uint8_t *,
                                               size_t, void *),
                                    void *user) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    mh->rx_cb   = cb;
    mh->rx_user = user;
}

int nff_port_mqtt_connect(nff_mqtt_handle_t *h,
                           const char *client_id,
                           const char *lwt_topic,
                           const char *lwt_payload) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    if (lwt_topic)   strncpy(mh->lwt_topic,   lwt_topic,   sizeof(mh->lwt_topic)   - 1);
    if (lwt_payload) strncpy(mh->lwt_payload,  lwt_payload, sizeof(mh->lwt_payload) - 1);

    /* Destroy existing client if reconnecting */
    if (mh->client) {
        esp_mqtt_client_destroy(mh->client);
        mh->client = NULL;
    }

    char uri[320];
    snprintf(uri, sizeof(uri), "mqtts://%s:%u", mh->host, (unsigned)mh->port);

    const esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = uri,
            .verification.certificate     = (const char *)mh->ca,
            .verification.certificate_len = mh->ca_len,
        },
        .credentials = {
            .client_id         = client_id,
            .authentication = {
                .certificate     = (const char *)mh->cert,
                .certificate_len = mh->cert_len,
                .key             = (const char *)mh->key,
                .key_len         = mh->key_len,
            },
        },
        .session = {
            .last_will = {
                .topic   = mh->lwt_topic,
                .msg     = mh->lwt_payload,
                .qos     = 1,
                .retain  = 1,
            },
        },
        /* Match the Arduino ports: ensure the RX/TX buffer fits OTA-sized
           commands. esp-mqtt defaults to 1024, but set it explicitly. */
        .buffer = {
            .size = NFF_MQTT_BUFFER_SIZE,
        },
    };

    mh->client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mh->client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, mh);
    esp_mqtt_client_start(mh->client);
    /* Wait up to 5 s for connect */
    for (int i = 0; i < 50 && !mh->connected; i++) vTaskDelay(pdMS_TO_TICKS(100));
    return mh->connected ? 0 : -1;
}

int nff_port_mqtt_subscribe(nff_mqtt_handle_t *h, const char *topic, int qos) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    if (!mh->client) return -1;
    return esp_mqtt_client_subscribe(mh->client, topic, qos) >= 0 ? 0 : -1;
}

int nff_port_mqtt_publish(nff_mqtt_handle_t *h, const char *topic,
                           const char *payload, int qos, bool retain) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    if (!mh->client) return -1;
    int msg_id = esp_mqtt_client_publish(mh->client, topic, payload,
                                          (int)strlen(payload), qos, (int)retain);
    return (msg_id >= 0) ? 0 : -1;
}

void nff_port_mqtt_loop(nff_mqtt_handle_t *h) {
    (void)h;
    /* esp-mqtt is event-driven and runs its own task — no polling needed */
}

bool nff_port_mqtt_is_connected(nff_mqtt_handle_t *h) {
    struct nff_mqtt_handle *mh = (struct nff_mqtt_handle *)h;
    return mh && mh->connected;
}

/* ------------------------------------------------------------------ */
/* NVS                                                                  */
/* ------------------------------------------------------------------ */

static nvs_handle_t get_nvs_handle(void) {
    static nvs_handle_t h = 0;
    if (h == 0) {
        nvs_flash_init();
        nvs_open("nff", NVS_READWRITE, &h);
    }
    return h;
}

int nff_port_nvs_set_str(const char *key, const char *value) {
    return (nvs_set_str(get_nvs_handle(), key, value) == ESP_OK) ? 0 : -1;
}

int nff_port_nvs_get_str(const char *key, char *out, size_t out_len) {
    return (nvs_get_str(get_nvs_handle(), key, out, &out_len) == ESP_OK) ? 0 : -1;
}

int nff_port_nvs_set_u32(const char *key, uint32_t value) {
    return (nvs_set_u32(get_nvs_handle(), key, value) == ESP_OK) ? 0 : -1;
}

int nff_port_nvs_get_u32(const char *key, uint32_t *out) {
    return (nvs_get_u32(get_nvs_handle(), key, out) == ESP_OK) ? 0 : -1;
}

int nff_port_nvs_erase_key(const char *key) {
    return (nvs_erase_key(get_nvs_handle(), key) == ESP_OK) ? 0 : -1;
}

int nff_port_nvs_commit(void) {
    return (nvs_commit(get_nvs_handle()) == ESP_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* OTA                                                                  */
/* ------------------------------------------------------------------ */

struct ota_handle_s { esp_ota_handle_t esp_h; const esp_partition_t *part; };

nff_ota_handle_t nff_port_ota_begin(size_t expected_size) {
    struct ota_handle_s *h = malloc(sizeof(*h));
    if (!h) return NFF_OTA_INVALID_HANDLE;
    h->part = esp_ota_get_next_update_partition(NULL);
    if (!h->part || esp_ota_begin(h->part, expected_size, &h->esp_h) != ESP_OK) {
        free(h);
        return NFF_OTA_INVALID_HANDLE;
    }
    return (nff_ota_handle_t)h;
}

int nff_port_ota_write(nff_ota_handle_t hh, const uint8_t *buf, size_t len) {
    struct ota_handle_s *h = (struct ota_handle_s *)hh;
    return (esp_ota_write(h->esp_h, buf, len) == ESP_OK) ? 0 : -1;
}

int nff_port_ota_end(nff_ota_handle_t hh) {
    struct ota_handle_s *h = (struct ota_handle_s *)hh;
    if (esp_ota_end(h->esp_h) != ESP_OK) { free(h); return -1; }
    int rc = (esp_ota_set_boot_partition(h->part) == ESP_OK) ? 0 : -1;
    free(h);
    return rc;
}

void nff_port_ota_abort(nff_ota_handle_t hh) {
    struct ota_handle_s *h = (struct ota_handle_s *)hh;
    esp_ota_abort(h->esp_h);
    free(h);
}

void nff_port_reboot(void) {
    esp_restart();
}

/* ------------------------------------------------------------------ */
/* HTTPS streaming (using esp_http_client)                             */
/* ------------------------------------------------------------------ */

#include "esp_http_client.h"

typedef struct {
    int (*chunk_cb)(const uint8_t *, size_t, void *);
    void *user_ctx;
    int   error;
} https_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    https_ctx_t *ctx = (https_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && !ctx->error) {
        int rc = ctx->chunk_cb((const uint8_t *)evt->data,
                                (size_t)evt->data_len, ctx->user_ctx);
        if (rc < 0) ctx->error = rc;
    }
    return ESP_OK;
}

int nff_port_https_get_stream(const char *url,
                               int (*chunk_cb)(const uint8_t *, size_t, void *),
                               void *user_ctx, uint32_t timeout_ms) {
    https_ctx_t ctx = { chunk_cb, user_ctx, 0 };
    esp_http_client_config_t cfg = {
        .url          = url,
        .event_handler = http_event_cb,
        .user_data     = &ctx,
        .timeout_ms   = (int)timeout_ms,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return -1;
    return ctx.error;
}

/* ------------------------------------------------------------------ */
/* SHA-256                                                              */
/* ------------------------------------------------------------------ */

nff_sha256_ctx_t nff_port_sha256_new(void) {
    mbedtls_sha256_context *c = malloc(sizeof(*c));
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
/* ECDSA-P256 verify                                                    */
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

#include "esp_heap_caps.h"
#include "esp_wifi.h"

void nff_port_get_diag_info(nff_diag_info_t *out) {
    out->free_heap     = (uint32_t)esp_get_free_heap_size();
    out->min_free_heap = (uint32_t)esp_get_minimum_free_heap_size();
    out->uptime_ms     = nff_port_millis();
    out->cpu_count     = 2;

    wifi_ap_record_t ap;
    out->wifi_rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                     ? (int32_t)ap.rssi : 0;
}

#include "esp_chip_info.h"
#include "esp_flash.h"

void nff_port_get_hw_info(nff_hw_info_t *out) {
    esp_chip_info_t info;
    esp_chip_info(&info);

    /* CONFIG_IDF_TARGET is already the canonical family ("esp32","esp32s3",...),
     * which is exactly the device_type vocabulary the OTA gate matches on. */
    snprintf(out->device_type, sizeof(out->device_type), "%s", CONFIG_IDF_TARGET);

    const char *model;
    switch (info.model) {
        case CHIP_ESP32:   model = "ESP32";    break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        case CHIP_ESP32C6: model = "ESP32-C6"; break;
        case CHIP_ESP32H2: model = "ESP32-H2"; break;
        default:           model = "ESP32";    break;
    }
    snprintf(out->chip_model, sizeof(out->chip_model), "%s", model);
    out->revision = (uint8_t)info.revision;
    out->cores    = (uint8_t)info.cores;

    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) != ESP_OK) flash = 0;
    out->flash_size = flash;
}

/* ------------------------------------------------------------------ */
/* Panic hook                                                           */
/* ------------------------------------------------------------------ */

#include "esp_system.h"

static void (*s_panic_hook)(const char *) = NULL;

static void IRAM_ATTR nff_esp_panic_handler(const esp_panic_info_t *info) {
    if (s_panic_hook) {
        char reason[64];
        snprintf(reason, sizeof(reason), "cause=%d pc=0x%08x",
                 (int)info->exception_cause,
                 (uint32_t)(uintptr_t)info->exception_addr);
        s_panic_hook(reason);
    }
    esp_default_panic_handler(info);
}

void nff_port_install_panic_hook(void (*fn)(const char *)) {
    s_panic_hook = fn;
    esp_set_panic_handler(nff_esp_panic_handler);
}

/* ------------------------------------------------------------------ */
/* AMP                                                                  */
/* ------------------------------------------------------------------ */

int nff_port_stall_core(int id) {
#if !CONFIG_FREERTOS_UNICORE
    esp_cpu_stall(id);
    return 0;
#else
    (void)id;
    return -1;
#endif
}

int nff_port_unstall_core(int id) {
#if !CONFIG_FREERTOS_UNICORE
    esp_cpu_unstall(id);
    return 0;
#else
    (void)id;
    return -1;
#endif
}

#endif /* ESP_PLATFORM && !ARDUINO */

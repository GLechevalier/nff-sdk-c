/**
 * nff_ota.c — HTTPS OTA firmware download with SHA-256 verification and rollback.
 *
 * Handles the "ota" command:
 *   1. Anti-downgrade version check
 *   2. HTTPS streaming download into OTA partition
 *   3. SHA-256 verification of downloaded image
 *   4. Boot partition switch + reboot
 *   5. On next boot: publish ota_result (committed/rolled_back) via NVS flag
 *
 * SHA-256 is computed incrementally during the download using mbedTLS
 * (linked by WiFi stack on all ESP targets; OpenSSL on POSIX).
 */

#include "nff_internal.h"
#include "nff_json.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* SHA-256 streaming state (platform-specific compute)                 */
/* ------------------------------------------------------------------ */

/* SHA-256 functions are declared in nff_port.h */

/* ------------------------------------------------------------------ */
/* Version comparison: "1.2.3" < "1.2.4" → true (anti-downgrade)      */
/* ------------------------------------------------------------------ */

static int version_parts(const char *v, int *maj, int *min, int *patch) {
    return (sscanf(v, "%d.%d.%d", maj, min, patch) == 3) ? 0 : -1;
}

static int version_le(const char *a, const char *b) {
    int a1=0, a2=0, a3=0, b1=0, b2=0, b3=0;
    if (version_parts(a, &a1, &a2, &a3) < 0) return 0;
    if (version_parts(b, &b1, &b2, &b3) < 0) return 0;
    if (a1 != b1) return (a1 < b1);
    if (a2 != b2) return (a2 < b2);
    return (a3 <= b3);
}

/* ------------------------------------------------------------------ */
/* Download context passed through chunk callback                       */
/* ------------------------------------------------------------------ */

typedef struct {
    nff_ota_handle_t  ota;
    nff_sha256_ctx_t  sha;
    size_t            written;
    int               error;
} ota_dl_ctx_t;

static int ota_chunk_cb(const uint8_t *buf, size_t len, void *user_ctx) {
    ota_dl_ctx_t *ctx = (ota_dl_ctx_t *)user_ctx;
    if (ctx->error) return -1;

    nff_port_sha256_update(ctx->sha, buf, len);

    int rc = nff_port_ota_write(ctx->ota, buf, len);
    if (rc != 0) {
        ctx->error = rc;
        return -1;
    }
    ctx->written += len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* OTA result publish (called after MQTT reconnect on new firmware)    */
/* ------------------------------------------------------------------ */

void nff_ota_check_pending_result(void) {
    if (!g_nff.pending_ota_result) return;
    g_nff.pending_ota_result = false;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"type\":\"ota_result\","
             "\"status\":\"%s\","
             "\"version\":\"%s\","
             "\"id\":\"%s\"}",
             g_nff.pending_ota_committed ? "committed" : "rolled_back",
             g_nff.pending_ota_version,
             g_nff.cfg->device_id);

    char topic[NFF_TOPIC_MAXLEN];
    nff_topic_response(&g_nff, topic);
    nff_port_mqtt_publish(g_nff.mqtt, topic, payload, 1, false);
}

/* ------------------------------------------------------------------ */
/* Main OTA command handler (called from nff_cmd.c)                    */
/* ------------------------------------------------------------------ */

void nff_ota_handle_cmd(const char *payload, char *resp, size_t resp_len) {
    char version[32]  = {0};
    char url[512]     = {0};
    char sha256hex[65] = {0};
    uint32_t size     = 0;

    if (nff_json_get_str(payload, strlen(payload), "version", version, sizeof(version)) != 0 ||
        nff_json_get_str(payload, strlen(payload), "url",     url,     sizeof(url))     != 0 ||
        nff_json_get_str(payload, strlen(payload), "sha256",  sha256hex, sizeof(sha256hex)) != 0) {
        snprintf(resp, resp_len, "{\"type\":\"error\",\"msg\":\"ota: missing fields\"}");
        return;
    }
    nff_json_get_u32(payload, strlen(payload), "size", &size);

    /* Anti-downgrade check */
    const char *fw = g_nff.cfg->fw_version ? g_nff.cfg->fw_version : "0.0.0";
    if (!version_le(fw, version)) {
        snprintf(resp, resp_len,
                 "{\"type\":\"error\",\"msg\":\"ota: downgrade rejected\","
                 "\"current\":\"%s\",\"requested\":\"%s\"}", fw, version);
        return;
    }

    /* Acknowledge receipt before download (download may take minutes) */
    char topic[NFF_TOPIC_MAXLEN];
    nff_topic_response(&g_nff, topic);
    snprintf(resp, resp_len,
             "{\"type\":\"ota_started\",\"version\":\"%s\",\"id\":\"%s\"}",
             version, g_nff.cfg->device_id);
    nff_port_mqtt_publish(g_nff.mqtt, topic, resp, 1, false);

    /* Mark state — nff_loop won't drive heartbeat during OTA */
    g_nff.state = NFF_STATE_OTA_ACTIVE;
    nff_log("nff: OTA start v%s", version);

    /* Convert expected SHA-256 hex to bytes */
    uint8_t expected_sha[32] = {0};
    for (int i = 0; i < 32; i++) {
        uint8_t hi, lo;
        char c = sha256hex[i * 2];
        hi = (uint8_t)((c>='a')? c-'a'+10 : (c>='A')? c-'A'+10 : c-'0');
        c  = sha256hex[i * 2 + 1];
        lo = (uint8_t)((c>='a')? c-'a'+10 : (c>='A')? c-'A'+10 : c-'0');
        expected_sha[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Begin OTA partition write */
    nff_ota_handle_t h = nff_port_ota_begin((size_t)size);
    if (h == NFF_OTA_INVALID_HANDLE) {
        snprintf(resp, resp_len, "{\"type\":\"error\",\"msg\":\"ota: begin failed\"}");
        g_nff.state = NFF_STATE_CONNECTED;
        return;
    }

    ota_dl_ctx_t ctx;
    ctx.ota     = h;
    ctx.sha     = nff_port_sha256_new();
    ctx.written = 0;
    ctx.error   = 0;

    int dl_rc = nff_port_https_get_stream(url, ota_chunk_cb, &ctx, 300000 /* 5 min */);

    /* Finalise SHA-256 */
    uint8_t actual_sha[32] = {0};
    nff_port_sha256_finish(ctx.sha, actual_sha);
    nff_port_sha256_free(ctx.sha);

    if (dl_rc != 0 || ctx.error != 0) {
        nff_port_ota_abort(h);
        nff_log("nff: OTA download failed rc=%d", dl_rc ? dl_rc : ctx.error);
        snprintf(resp, resp_len, "{\"type\":\"error\",\"msg\":\"ota: download failed\"}");
        g_nff.state = NFF_STATE_CONNECTED;
        /* Response already published above; clear for caller */
        resp[0] = '\0';
        return;
    }

    /* SHA-256 mismatch → abort */
    if (memcmp(actual_sha, expected_sha, 32) != 0) {
        nff_port_ota_abort(h);
        nff_log("nff: OTA SHA-256 mismatch");
        snprintf(resp, resp_len, "{\"type\":\"error\",\"msg\":\"ota: sha256 mismatch\"}");
        nff_port_mqtt_publish(g_nff.mqtt, topic, resp, 1, false);
        resp[0] = '\0';
        g_nff.state = NFF_STATE_CONNECTED;
        return;
    }

    /* Finalise partition — this marks the new partition as bootable */
    if (nff_port_ota_end(h) != 0) {
        nff_log("nff: OTA end failed");
        snprintf(resp, resp_len, "{\"type\":\"error\",\"msg\":\"ota: end failed\"}");
        nff_port_mqtt_publish(g_nff.mqtt, topic, resp, 1, false);
        resp[0] = '\0';
        g_nff.state = NFF_STATE_CONNECTED;
        return;
    }

    /* Persist OTA result intent to NVS so new firmware can publish it */
    nff_port_nvs_set_str("ota_pending",   "1");
    nff_port_nvs_set_str("ota_version",   version);
    nff_port_nvs_set_str("ota_committed", "1");  /* assume committed; rollback clears */
    nff_port_nvs_commit();

    nff_log("nff: OTA download complete, rebooting");

    /* Small delay to let MQTT ACK flush */
    nff_port_delay_ms(500);
    nff_port_reboot();

    /* Never reached */
    resp[0] = '\0';
}

/**
 * nff_cmd.c — Command receive, parse, verify, dispatch.
 *
 * Called by nff_mqtt when a message arrives on the cmd topic.
 * Verifies the ECDSA signature, then dispatches to built-in or
 * user-registered handlers, and publishes the result on the response topic.
 */

#include "nff_internal.h"
#include "nff_json.h"
#include <string.h>
#include <stdio.h>

/* Forward declaration from nff_security.c */
int nff_security_verify_cmd(const char *payload, size_t plen);

/* ------------------------------------------------------------------ */
/* Built-in handlers                                                    */
/* ------------------------------------------------------------------ */

static void handle_ping(const char *payload, char *resp, size_t resp_len, void *ctx) {
    (void)payload; (void)ctx;
    snprintf(resp, resp_len,
             "{\"type\":\"pong\",\"id\":\"%s\",\"fw\":\"%s\"}",
             g_nff.cfg->device_id,
             g_nff.cfg->fw_version ? g_nff.cfg->fw_version : "");
}

static void handle_reboot(const char *payload, char *resp, size_t resp_len, void *ctx) {
    (void)payload; (void)ctx;
    snprintf(resp, resp_len,
             "{\"type\":\"rebooting\",\"id\":\"%s\"}",
             g_nff.cfg->device_id);
    /* Publish the acknowledgement before rebooting */
    char topic[NFF_TOPIC_MAXLEN];
    nff_topic_response(&g_nff, topic);
    nff_port_mqtt_publish(g_nff.mqtt, topic, resp, 1, false);
    nff_port_delay_ms(200);
    nff_port_reboot();
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

void nff_cmd_dispatch(const char *topic, const uint8_t *payload, size_t len) {
    (void)topic;

    if (!payload || len == 0) return;

    /* Work on a null-terminated copy (payload may not be null-terminated) */
    static char buf[512];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = '\0';

    /* Verify signature first — reject unsigned or replayed commands */
    if (nff_security_verify_cmd(buf, len) != NFF_OK) {
        nff_log("nff: cmd rejected (security)");
        return;
    }

    /* Extract action field */
    char action[32] = {0};
    if (nff_json_get_str(buf, len, "action", action, sizeof(action)) != 0) {
        nff_log("nff: cmd missing action field");
        return;
    }

    /* Response buffer */
    static char resp[NFF_RESPONSE_MAXLEN];
    resp[0] = '\0';

    /* Built-in handlers */
    if (strcmp(action, "ping") == 0) {
        handle_ping(buf, resp, sizeof(resp), NULL);
    } else if (strcmp(action, "reboot") == 0) {
        handle_reboot(buf, resp, sizeof(resp), NULL);
        return; /* handle_reboot publishes and reboots — never reaches here */
    } else if (strcmp(action, "ota") == 0) {
        /* Forwarded to nff_ota — defined there to keep OTA logic co-located */
        extern void nff_ota_handle_cmd(const char *payload, char *resp, size_t resp_len);
        nff_ota_handle_cmd(buf, resp, sizeof(resp));
    } else if (strcmp(action, "diag") == 0) {
        extern void nff_diag_handle_cmd(const char *payload, char *resp, size_t resp_len);
        nff_diag_handle_cmd(buf, resp, sizeof(resp));
    } else {
        /* User-registered commands */
        int found = 0;
        nff_port_mutex_lock(g_nff.lock);
        for (int i = 0; i < g_nff.num_cmds; i++) {
            if (strcmp(g_nff.cmds[i].name, action) == 0) {
                nff_cmd_handler_t h = g_nff.cmds[i].handler;
                void *ctx            = g_nff.cmds[i].user_ctx;
                nff_port_mutex_unlock(g_nff.lock);
                h(buf, resp, sizeof(resp), ctx);
                found = 1;
                break;
            }
        }
        if (!found) {
            nff_port_mutex_unlock(g_nff.lock);
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"error\",\"msg\":\"unknown action\",\"action\":\"%s\"}",
                     action);
        }
    }

    /* Publish response (skip if handler left resp empty) */
    if (resp[0] != '\0') {
        char rtopic[NFF_TOPIC_MAXLEN];
        nff_topic_response(&g_nff, rtopic);
        nff_port_mqtt_publish(g_nff.mqtt, rtopic, resp, 1, false);
    }
}

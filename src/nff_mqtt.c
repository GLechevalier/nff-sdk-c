/**
 * nff_mqtt.c — MQTT transport wrapper.
 *
 * Creates the port MQTT handle, configures mTLS and LWT, and drives the
 * reconnect loop. All received messages are forwarded to nff_cmd_dispatch.
 */

#include "nff_internal.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* RX callback — called by the port's MQTT client on message arrival   */
/* ------------------------------------------------------------------ */

static void mqtt_rx_cb(const char *topic, const uint8_t *payload,
                        size_t len, void *user) {
    (void)user;
    nff_cmd_dispatch(topic, payload, len);
}

/* ------------------------------------------------------------------ */
/* nff_mqtt_init — allocate handle and configure                        */
/* ------------------------------------------------------------------ */

void nff_mqtt_init(void) {
    if (g_nff.mqtt) return;  /* already initialised */

    g_nff.mqtt = nff_port_mqtt_create();
    if (!g_nff.mqtt) return;

    nff_port_mqtt_set_server(g_nff.mqtt,
                              g_nff.cfg->broker_host,
                              g_nff.cfg->broker_port);

    nff_port_mqtt_set_tls(g_nff.mqtt,
                           g_nff.cfg->ca_cert,    g_nff.cfg->ca_cert_len,
                           g_nff.cfg->client_cert, g_nff.cfg->client_cert_len,
                           g_nff.cfg->client_key,  g_nff.cfg->client_key_len);

    nff_port_mqtt_set_rx_callback(g_nff.mqtt, mqtt_rx_cb, NULL);

    /* Register LWT — broker publishes this automatically on unclean disconnect */
    char lwt_topic[NFF_TOPIC_MAXLEN];
    nff_topic_heartbeat(&g_nff, lwt_topic);

    static char lwt_payload[128];
    snprintf(lwt_payload, sizeof(lwt_payload),
             "{\"status\":\"offline\",\"id\":\"%s\"}",
             g_nff.cfg->device_id);

    /* LWT is passed to connect — store for reconnect */
    /* nff_port_mqtt_connect wraps LWT setup in the platform layer when
       lwt_topic/lwt_payload are supplied. Pass them now via connect. */
    nff_port_mqtt_connect(g_nff.mqtt, g_nff.cfg->device_id,
                           lwt_topic, lwt_payload);

    if (nff_port_mqtt_is_connected(g_nff.mqtt)) {
        /* Subscribe to the device's command topic at QoS 1 */
        char cmd_topic[NFF_TOPIC_MAXLEN];
        nff_topic_cmd(&g_nff, cmd_topic);
        nff_port_mqtt_subscribe(g_nff.mqtt, cmd_topic, 1);
    }
}

/* ------------------------------------------------------------------ */
/* nff_mqtt_tick — called from nff_loop every iteration                */
/* ------------------------------------------------------------------ */

void nff_mqtt_tick(void) {
    if (!g_nff.mqtt) return;

    if (nff_port_mqtt_is_connected(g_nff.mqtt)) {
        /* Service the MQTT event loop */
        nff_port_mqtt_loop(g_nff.mqtt);
        if (g_nff.state == NFF_STATE_CONNECTING) {
            g_nff.state = NFF_STATE_CONNECTED;
            nff_heartbeat_on_connect();
            nff_crash_check_and_report();
            nff_ota_check_pending_result();
            /* Reset reconnect back-off */
            g_nff.reconnect_delay_ms = 1000;
            g_nff.reconnect_next_ms  = 0;
        }
    } else {
        /* Disconnected: mark state, attempt reconnect after back-off */
        if (g_nff.state == NFF_STATE_CONNECTED ||
            g_nff.state == NFF_STATE_OTA_ACTIVE) {
            g_nff.state = NFF_STATE_CONNECTING;
            nff_log("nff: MQTT disconnected, back-off %lu ms",
                    (unsigned long)g_nff.reconnect_delay_ms);
        }

        uint32_t now = nff_port_millis();
        if (now >= g_nff.reconnect_next_ms) {
            /* Attempt reconnect */
            char lwt_topic[NFF_TOPIC_MAXLEN];
            nff_topic_heartbeat(&g_nff, lwt_topic);
            static char lwt_payload[128];
            snprintf(lwt_payload, sizeof(lwt_payload),
                     "{\"status\":\"offline\",\"id\":\"%s\"}",
                     g_nff.cfg->device_id);

            nff_port_mqtt_connect(g_nff.mqtt, g_nff.cfg->device_id,
                                   lwt_topic, lwt_payload);

            if (nff_port_mqtt_is_connected(g_nff.mqtt)) {
                char cmd_topic[NFF_TOPIC_MAXLEN];
                nff_topic_cmd(&g_nff, cmd_topic);
                nff_port_mqtt_subscribe(g_nff.mqtt, cmd_topic, 1);
                g_nff.state = NFF_STATE_CONNECTED;
                /* Reconnect callbacks */
                nff_heartbeat_on_connect();
                nff_crash_check_and_report();
                nff_ota_check_pending_result();
                g_nff.reconnect_delay_ms = 1000;
                g_nff.reconnect_next_ms  = 0;
            } else {
                /* Advance back-off */
                g_nff.reconnect_next_ms = now + g_nff.reconnect_delay_ms;
                static const uint32_t steps[] = {1000,2000,4000,8000,16000,32000,60000};
                for (int i = 0; i + 1 < (int)(sizeof(steps)/sizeof(steps[0])); i++) {
                    if (g_nff.reconnect_delay_ms == steps[i]) {
                        g_nff.reconnect_delay_ms = steps[i + 1];
                        break;
                    }
                }
                /* Saturate at 60 s if already at max */
                if (g_nff.reconnect_delay_ms > 60000) g_nff.reconnect_delay_ms = 60000;
            }
        } else {
            /* Not yet time to retry — drive the MQTT loop anyway */
            nff_port_mqtt_loop(g_nff.mqtt);
        }
    }
}

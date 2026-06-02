/**
 * nff_heartbeat.c — Retained presence heartbeat and LWT.
 *
 * Publishes a retained QoS-1 heartbeat to nff/devices/{id}/heartbeat on
 * every connect and every heartbeat_interval_s seconds thereafter.
 * The LWT (offline payload) is registered at connect time via nff_mqtt.
 */

#include "nff_internal.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Heartbeat publish                                                    */
/* ------------------------------------------------------------------ */

static void publish_heartbeat(void) {
    if (!g_nff.mqtt || !nff_port_mqtt_is_connected(g_nff.mqtt)) return;

    nff_diag_info_t di;
    nff_port_get_diag_info(&di);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"online\","
             "\"id\":\"%s\","
             "\"fw\":\"%s\","
             "\"build\":\"%s\","
             "\"heap\":%lu,"
             "\"uptime\":%lu}",
             g_nff.cfg->device_id,
             g_nff.cfg->fw_version  ? g_nff.cfg->fw_version  : "",
             g_nff.cfg->build_id    ? g_nff.cfg->build_id    : "",
             (unsigned long)di.free_heap,
             (unsigned long)di.uptime_ms);

    char topic[NFF_TOPIC_MAXLEN];
    nff_topic_heartbeat(&g_nff, topic);
    nff_port_mqtt_publish(g_nff.mqtt, topic, payload, 1 /* QoS 1 */, true /* retain */);
}

/* ------------------------------------------------------------------ */
/* Module hooks                                                         */
/* ------------------------------------------------------------------ */

void nff_heartbeat_init(void) {
    /* Nothing to allocate; timer state lives in g_nff */
}

void nff_heartbeat_on_connect(void) {
    publish_heartbeat();
    uint32_t hb_s = g_nff.cfg->heartbeat_interval_s
                    ? g_nff.cfg->heartbeat_interval_s
                    : NFF_HEARTBEAT_INTERVAL_S;
    g_nff.heartbeat_next_ms = nff_port_millis() + hb_s * 1000;
}

void nff_heartbeat_tick(void) {
    uint32_t now = nff_port_millis();
    if (now >= g_nff.heartbeat_next_ms) {
        publish_heartbeat();
        uint32_t hb_s = g_nff.cfg->heartbeat_interval_s
                        ? g_nff.cfg->heartbeat_interval_s
                        : NFF_HEARTBEAT_INTERVAL_S;
        g_nff.heartbeat_next_ms = now + hb_s * 1000;
    }
}

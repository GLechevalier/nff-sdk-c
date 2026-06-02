/* nff_internal.h — shared state, not part of the public API */
#ifndef NFF_INTERNAL_H
#define NFF_INTERNAL_H

#include "nff.h"
#include "nff_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Global context (defined in nff_core.c)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const nff_config_t *cfg;
    nff_state_t         state;
    nff_mutex_t         lock;

    /* MQTT */
    nff_mqtt_handle_t  *mqtt;

    /* Reconnect backoff */
    uint32_t            reconnect_delay_ms;   /* current back-off interval */
    uint32_t            reconnect_next_ms;    /* nff_port_millis() target */

    /* Heartbeat timer */
    uint32_t            heartbeat_next_ms;

    /* Pending OTA result to publish after MQTT reconnect */
    bool                pending_ota_result;
    char                pending_ota_version[32];
    bool                pending_ota_committed; /* true=committed, false=rolled_back */

    /* User command registry */
    struct {
        const char        *name;
        nff_cmd_handler_t  handler;
        void              *user_ctx;
    } cmds[NFF_MAX_USER_CMDS];
    int num_cmds;
} nff_ctx_t;

extern nff_ctx_t g_nff;

/* ------------------------------------------------------------------ */
/* Topic builder helpers (inlined for code size)                       */
/* ------------------------------------------------------------------ */

#include <string.h>
#include <stdio.h>

static inline void nff_topic_cmd(const nff_ctx_t *c, char *buf) {
    snprintf(buf, NFF_TOPIC_MAXLEN, "nff/devices/%s/cmd", c->cfg->device_id);
}
static inline void nff_topic_response(const nff_ctx_t *c, char *buf) {
    snprintf(buf, NFF_TOPIC_MAXLEN, "nff/devices/%s/response", c->cfg->device_id);
}
static inline void nff_topic_heartbeat(const nff_ctx_t *c, char *buf) {
    snprintf(buf, NFF_TOPIC_MAXLEN, "nff/devices/%s/heartbeat", c->cfg->device_id);
}
static inline void nff_topic_crash(const nff_ctx_t *c, char *buf) {
    snprintf(buf, NFF_TOPIC_MAXLEN, "nff/devices/%s/crash", c->cfg->device_id);
}

/* ------------------------------------------------------------------ */
/* Internal module init functions                                       */
/* ------------------------------------------------------------------ */

void nff_mqtt_init(void);
void nff_mqtt_tick(void);   /* called from nff_loop/task — reconnect + loop */

void nff_heartbeat_init(void);
void nff_heartbeat_on_connect(void);
void nff_heartbeat_tick(void);

void nff_crash_init(void);  /* install panic hook, check previous boot */
void nff_crash_check_and_report(void); /* called after MQTT connect */

void nff_cmd_dispatch(const char *topic, const uint8_t *payload, size_t len);

void nff_ota_check_pending_result(void); /* publish committed/rolled_back if flag set */

#ifdef __cplusplus
}
#endif

#endif /* NFF_INTERNAL_H */

/**
 * nff_crash.c — Panic hook, RTC circular log, boot-time crash report.
 *
 * Three-layer crash pipeline:
 *   1. Panic hook (IRAM, synchronous): writes metadata to NVS.
 *   2. RTC memory circular log: nff_log() appends here; survives reset.
 *   3. Boot-time: detect crash via esp_reset_reason(), assemble JSON,
 *      publish retained QoS-1 to crash topic, then erase NVS keys.
 *
 * On POSIX (host tests): panic hook is a no-op, RTC log is in normal RAM,
 * reset reason is simulated via nff_port_nvs.
 */

#include "nff_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* RTC circular log                                                     */
/* ------------------------------------------------------------------ */

/*
 * On ESP32 these must be placed in RTC_NOINIT_ATTR. On POSIX and other
 * platforms they are normal static variables (no warm-boot survival, but
 * the logic is exercisable).
 *
 * Platform ports that support RTC memory should redeclare these with
 * RTC_NOINIT_ATTR and include the platform-specific header.
 */

#if defined(ESP_PLATFORM)
#  include "esp_attr.h"
#  define NFF_RTC_ATTR RTC_NOINIT_ATTR
#else
#  define NFF_RTC_ATTR
#endif

#define NFF_RTC_MAGIC 0xDEADBEEFu

NFF_RTC_ATTR static char     s_rtc_log[NFF_LOG_LINES][NFF_LOG_LINE_LEN];
NFF_RTC_ATTR static uint32_t s_rtc_head;
NFF_RTC_ATTR static uint32_t s_rtc_magic;

static void rtc_log_init(void) {
    if (s_rtc_magic != NFF_RTC_MAGIC) {
        memset(s_rtc_log, 0, sizeof(s_rtc_log));
        s_rtc_head  = 0;
        s_rtc_magic = NFF_RTC_MAGIC;
    }
}

/* Append a line to the RTC circular buffer. Called from nff_log(). */
void nff_crash_log(const char *line) {
    rtc_log_init();
    uint32_t slot = s_rtc_head % NFF_LOG_LINES;
    strncpy(s_rtc_log[slot], line, NFF_LOG_LINE_LEN - 1);
    s_rtc_log[slot][NFF_LOG_LINE_LEN - 1] = '\0';
    s_rtc_head++;

    /* Also output to UART via platform log */
#if defined(ESP_PLATFORM)
    esp_rom_printf("[nff] %s\n", line);
#elif defined(ARDUINO)
    Serial.println(line);
#else
    fprintf(stderr, "[nff] %s\n", line);
#endif
}

/* ------------------------------------------------------------------ */
/* Panic hook                                                           */
/* ------------------------------------------------------------------ */

static void panic_handler(const char *reason) {
    /* This runs in ISR / panic context — only NVS writes are safe */
    uint32_t count = 0;
    nff_port_nvs_get_u32("crash_count", &count);
    count++;
    nff_port_nvs_set_u32("crash_count", count);

    if (reason) {
        /* Truncate to fit NVS string limit (typically 256 chars) */
        char trunc[128];
        strncpy(trunc, reason, sizeof(trunc) - 1);
        trunc[sizeof(trunc) - 1] = '\0';
        nff_port_nvs_set_str("crash_reason", trunc);
    }

    nff_port_nvs_set_u32("crash_uptime", nff_port_millis());
    nff_port_nvs_commit();
    /* The platform's default panic handler continues from here */
}

/* ------------------------------------------------------------------ */
/* nff_crash_init                                                       */
/* ------------------------------------------------------------------ */

void nff_crash_init(void) {
    rtc_log_init();
    nff_port_install_panic_hook(panic_handler);
}

/* ------------------------------------------------------------------ */
/* nff_crash_check_and_report (called after MQTT connect)              */
/* ------------------------------------------------------------------ */

/*
 * On ESP32 we query esp_reset_reason(). On POSIX we check an NVS key
 * "crash_simulate" that tests can set to trigger the code path.
 */
static int was_crash_boot(void) {
#if defined(ESP_PLATFORM)
    esp_reset_reason_t r = esp_reset_reason();
    return (r == ESP_RST_PANIC     ||
            r == ESP_RST_INT_WDT   ||
            r == ESP_RST_TASK_WDT  ||
            r == ESP_RST_WDT);
#else
    char val[4] = {0};
    return (nff_port_nvs_get_str("crash_simulate", val, sizeof(val)) == 0
            && val[0] == '1');
#endif
}

void nff_crash_check_and_report(void) {
    if (!was_crash_boot()) return;

    /* Collect metadata from NVS */
    char reason[128]  = {0};
    uint32_t count    = 0;
    uint32_t uptime   = 0;
    nff_port_nvs_get_str("crash_reason",  reason,  sizeof(reason));
    nff_port_nvs_get_u32("crash_count",  &count);
    nff_port_nvs_get_u32("crash_uptime", &uptime);

    /* Build pre-crash log JSON array from RTC circular buffer */
    static char log_json[NFF_LOG_LINES * (NFF_LOG_LINE_LEN + 4)];
    int pos = 0;
    pos += snprintf(log_json + pos, sizeof(log_json) - (size_t)pos, "[");
    uint32_t total = (s_rtc_head < NFF_LOG_LINES) ? s_rtc_head : NFF_LOG_LINES;
    uint32_t start = (s_rtc_head >= NFF_LOG_LINES) ? s_rtc_head - NFF_LOG_LINES : 0;
    for (uint32_t i = 0; i < total && pos < (int)sizeof(log_json) - 4; i++) {
        uint32_t slot = (start + i) % NFF_LOG_LINES;
        if (s_rtc_log[slot][0] == '\0') continue;
        pos += snprintf(log_json + pos, sizeof(log_json) - (size_t)pos,
                        "%s\"%s\"",
                        (i == 0) ? "" : ",",
                        s_rtc_log[slot]);
    }
    pos += snprintf(log_json + pos, sizeof(log_json) - (size_t)pos, "]");

    /* Assemble crash report JSON */
    static char report[2048];
    snprintf(report, sizeof(report),
             "{\"type\":\"crash\","
             "\"id\":\"%s\","
             "\"fw\":\"%s\","
             "\"build\":\"%s\","
             "\"reason\":\"%s\","
             "\"crash_count\":%lu,"
             "\"uptime_ms\":%lu,"
             "\"pre_crash_logs\":%s}",
             g_nff.cfg->device_id,
             g_nff.cfg->fw_version ? g_nff.cfg->fw_version : "",
             g_nff.cfg->build_id   ? g_nff.cfg->build_id   : "",
             reason,
             (unsigned long)count,
             (unsigned long)uptime,
             log_json);

    /* Publish retained QoS-1 — broker holds this indefinitely */
    char topic[NFF_TOPIC_MAXLEN];
    nff_topic_crash(&g_nff, topic);
    nff_port_mqtt_publish(g_nff.mqtt, topic, report, 1, true);

    /* Erase NVS crash keys so next clean boot doesn't re-report */
    nff_port_nvs_erase_key("crash_reason");
    nff_port_nvs_erase_key("crash_uptime");
    nff_port_nvs_erase_key("crash_simulate");
    /* Keep crash_count — cumulative counter across all boots */
    nff_port_nvs_commit();

    /* Reset RTC log head so next boot starts fresh */
    s_rtc_head = 0;
}

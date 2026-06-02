/**
 * test_ota_rollback.c — Unit tests for OTA NVS pending-result flag.
 *
 * Tests:
 *  1. nff_init reads "ota_pending" NVS key and sets g_nff.pending_ota_result
 *  2. After connect, nff_ota_check_pending_result publishes "committed" or "rolled_back"
 *  3. NVS keys are cleared after the report is published
 *  4. Anti-downgrade: OTA command with version <= current is rejected
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "nff.h"
#include "nff_port.h"
#include "nff_internal.h"

/* Mock ECDSA — always valid */
int nff_port_ecdsa_p256_verify(const uint8_t *a, const uint8_t *b, size_t c,
                                const uint8_t *d, size_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    return 0;
}

static const uint8_t NFF_CLIENT_CERT_DER[]    = {0};
static const uint8_t NFF_CLIENT_KEY_DER[]     = {0};
static const uint8_t NFF_CA_CERT_DER[]        = {0};
static const uint8_t NFF_CMD_VERIFY_KEY_DER[] = {0x04,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
#define NFF_CLIENT_CERT_LEN  sizeof(NFF_CLIENT_CERT_DER)
#define NFF_CLIENT_KEY_LEN   sizeof(NFF_CLIENT_KEY_DER)
#define NFF_CA_CERT_LEN      sizeof(NFF_CA_CERT_DER)
#define NFF_CMD_VERIFY_KEY_LEN 65
#define NFF_FW_VERSION "2.0.0"
#define NFF_BUILD_ID   "aabbccdd11223344"

extern nff_ctx_t g_nff;
extern void nff_port_posix_reset_nvs(void);
extern void nff_port_posix_clear_published(nff_mqtt_handle_t *h);
extern const char *nff_port_posix_get_published(nff_mqtt_handle_t *h, const char *topic);
extern void nff_security_reset_nonce_ring(void);
extern void nff_ota_check_pending_result(void);

/* Initialise without resetting NVS — call after populating NVS to simulate a reboot */
static void do_init(void) {
    nff_security_reset_nonce_ring();
    static nff_config_t cfg = NFF_CONFIG_INITIALIZER("test-device", "localhost");
    cfg.cmd_verify_key     = NFF_CMD_VERIFY_KEY_DER;
    cfg.cmd_verify_key_len = NFF_CMD_VERIFY_KEY_LEN;
    nff_init(&cfg);
    nff_connect();
}

/* Full clean init — resets NVS before initialising (clean-boot scenario) */
static void clean_init(void) {
    nff_port_posix_reset_nvs();
    do_init();
}

static void test_pending_committed(void) {
    /* Reset NVS, populate keys (simulating nff_ota writing before reboot), then init */
    nff_port_posix_reset_nvs();
    nff_port_nvs_set_str("ota_pending",   "1");
    nff_port_nvs_set_str("ota_version",   "2.1.0");
    nff_port_nvs_set_str("ota_committed", "1");
    nff_port_nvs_commit();

    /* do_init() calls nff_connect() which calls nff_ota_check_pending_result() internally.
       By the time do_init() returns, the result has been published and the flag cleared. */
    do_init();

    /* Verify the result was published during connect */
    const char *resp = nff_port_posix_get_published(g_nff.mqtt,
                           "nff/devices/test-device/response");
    assert(resp != NULL);
    assert(strstr(resp, "\"ota_result\"") != NULL);
    assert(strstr(resp, "\"committed\"")  != NULL);
    assert(strstr(resp, "2.1.0")          != NULL);

    /* Flag must be false — cleared by the internal call */
    assert(g_nff.pending_ota_result == false);

    /* NVS keys were cleared during nff_init */
    char val[8] = {0};
    assert(nff_port_nvs_get_str("ota_pending", val, sizeof(val)) != 0);

    printf("PASS: pending committed OTA result published\n");
}

static void test_pending_rolled_back(void) {
    nff_port_posix_reset_nvs();
    nff_port_nvs_set_str("ota_pending",   "1");
    nff_port_nvs_set_str("ota_version",   "1.9.0");
    nff_port_nvs_set_str("ota_committed", "0");  /* "0" = rolled_back */
    nff_port_nvs_commit();

    do_init();  /* publishes ota_result=rolled_back during nff_connect */

    const char *resp = nff_port_posix_get_published(g_nff.mqtt,
                           "nff/devices/test-device/response");
    assert(resp != NULL);
    assert(strstr(resp, "\"rolled_back\"") != NULL);
    assert(g_nff.pending_ota_result == false);

    printf("PASS: pending rolled_back OTA result published\n");
}

static void test_no_pending_no_publish(void) {
    clean_init();  /* clean boot — no NVS keys, nothing should be published */

    /* Nothing on the response topic from OTA — connect publishes heartbeat only */
    const char *resp = nff_port_posix_get_published(g_nff.mqtt,
                           "nff/devices/test-device/response");
    assert(resp == NULL);
    printf("PASS: no pending OTA → nothing published\n");
}

static void test_ota_cmd_downgrade_rejected(void) {
    /* Send OTA command with version 1.0.0 when current is 2.0.0 */
    nff_security_reset_nonce_ring();
    nff_port_posix_clear_published(g_nff.mqtt);

    char cmd_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "nff/devices/test-device/cmd");

    char json[512];
    snprintf(json, sizeof(json),
             "{\"action\":\"ota\","
             "\"version\":\"1.0.0\","
             "\"url\":\"https://cdn.example.com/fw.bin\","
             "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
             "\"size\":100,"
             "\"nonce\":\"deadbeef\","
             "\"timestamp\":0,"
             "\"cmd_sig\":\"3006020101020101\"}");

    extern void nff_port_posix_inject_mqtt(nff_mqtt_handle_t *, const char *, const char *);
    nff_port_posix_inject_mqtt(g_nff.mqtt, cmd_topic, json);

    const char *resp = nff_port_posix_get_published(g_nff.mqtt,
                           "nff/devices/test-device/response");
    assert(resp != NULL);
    assert(strstr(resp, "\"error\"")     != NULL);
    assert(strstr(resp, "downgrade")     != NULL);
    printf("PASS: OTA downgrade rejected\n");
}

int main(void) {
    test_pending_committed();
    test_pending_rolled_back();
    test_no_pending_no_publish();
    test_ota_cmd_downgrade_rejected();

    printf("All OTA rollback tests passed.\n");
    return 0;
}

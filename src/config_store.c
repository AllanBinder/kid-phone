#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Optional developer override (gitignored). The build works without it. */
#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "config";
static const char *NS = "kidphone";

static app_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

static void load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* namespace does not exist yet: empty config */
    }
    get_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    get_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    get_str(h, "sip_domain", s_cfg.sip_domain, sizeof(s_cfg.sip_domain));
    get_str(h, "sip_user", s_cfg.sip_user, sizeof(s_cfg.sip_user));
    get_str(h, "sip_pass", s_cfg.sip_pass, sizeof(s_cfg.sip_pass));
    get_str(h, "caller_id", s_cfg.caller_id, sizeof(s_cfg.caller_id));
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "c%d_name", i);
        get_str(h, k, s_cfg.contacts[i].name, sizeof(s_cfg.contacts[i].name));
        snprintf(k, sizeof(k), "c%d_num", i);
        get_str(h, k, s_cfg.contacts[i].number, sizeof(s_cfg.contacts[i].number));
    }
    uint8_t p = 0;
    nvs_get_u8(h, "provisioned", &p);
    /* Only trust the flag if the SIP account and contact 0 are really there. */
    s_cfg.provisioned = p == 1 && s_cfg.sip_domain[0] && s_cfg.sip_user[0] && s_cfg.sip_pass[0] &&
                        s_cfg.contacts[0].number[0];
    nvs_close(h);
}

static size_t copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strnlen(src, cap - 1) : 0;
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
    return n;
}

/* secrets.h.example placeholders are not real values. */
static bool __attribute__((unused)) is_placeholder(const char *v)
{
    return !v[0] || strstr(v, "your-") || strstr(v, "yourname.") || strcmp(v, "sip-credential-password") == 0;
}

/* Seed empty NVS from secrets.h (bench boards). After a factory reset this
 * runs again on the next boot, so a board built with secrets.h comes back
 * configured; build without secrets.h to test the real setup flow. */
static void seed_from_secrets(void)
{
#ifdef WIFI_SSID
    if (!s_cfg.wifi_ssid[0] && !is_placeholder(WIFI_SSID)) {
        ESP_LOGI(TAG, "seeding WiFi credentials from secrets.h");
        config_set_wifi(WIFI_SSID, WIFI_PASS);
    }
#endif
#if defined(SIP_DOMAIN) && defined(SIP_USER) && defined(SIP_PASSWORD) && defined(TEST_DEST_NUMBER)
    if (!s_cfg.provisioned && !is_placeholder(SIP_DOMAIN) && !is_placeholder(SIP_PASSWORD)) {
        ESP_LOGI(TAG, "seeding SIP account and contact 0 from secrets.h");
        cfg_contact_t c = {0};
        copy_str(c.name, sizeof(c.name), "Test");
        copy_str(c.number, sizeof(c.number), TEST_DEST_NUMBER);
#ifdef CALLER_ID
        const char *cid = CALLER_ID;
#else
        const char *cid = "";
#endif
        config_set_twilio(SIP_DOMAIN, SIP_USER, SIP_PASSWORD, cid, &c, 1);
    }
#endif
}

void config_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    assert(s_lock);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    lock();
    load();
    unlock();
    seed_from_secrets();

    const char *n = s_cfg.contacts[0].number;
    size_t nl = strlen(n);
    ESP_LOGI(TAG, "config: wifi %s, twilio %s, contact 0 %s%s", s_cfg.wifi_ssid[0] ? "set" : "NOT SET",
             s_cfg.provisioned ? "provisioned" : "NOT SET UP", nl > 4 ? "..." : (nl ? "" : "none"),
             nl > 4 ? n + nl - 4 : n);
}

void config_get(app_cfg_t *out)
{
    lock();
    *out = s_cfg;
    unlock();
}

void config_wipe(app_cfg_t *c)
{
    mbedtls_platform_zeroize(c, sizeof(*c));
}

bool config_has_wifi(void)
{
    lock();
    bool r = s_cfg.wifi_ssid[0] != '\0';
    unlock();
    return r;
}

bool config_is_provisioned(void)
{
    lock();
    bool r = s_cfg.provisioned;
    unlock();
    return r;
}

bool config_get_contact(int idx, cfg_contact_t *out)
{
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        return false;
    }
    lock();
    *out = s_cfg.contacts[idx];
    unlock();
    return out->number[0] != '\0';
}

esp_err_t config_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid);
    copy_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), pass);
    err = nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_pass", s_cfg.wifi_pass);
    }
    unlock();
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving WiFi credentials failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_set_twilio(const char *sip_domain, const char *sip_user, const char *sip_pass,
                            const char *caller_id, const cfg_contact_t *contacts, int n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    copy_str(s_cfg.sip_domain, sizeof(s_cfg.sip_domain), sip_domain);
    copy_str(s_cfg.sip_user, sizeof(s_cfg.sip_user), sip_user);
    copy_str(s_cfg.sip_pass, sizeof(s_cfg.sip_pass), sip_pass);
    copy_str(s_cfg.caller_id, sizeof(s_cfg.caller_id), caller_id);
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        if (i < n) {
            s_cfg.contacts[i] = contacts[i];
        } else {
            memset(&s_cfg.contacts[i], 0, sizeof(s_cfg.contacts[i]));
        }
    }
    err |= nvs_set_str(h, "sip_domain", s_cfg.sip_domain);
    err |= nvs_set_str(h, "sip_user", s_cfg.sip_user);
    err |= nvs_set_str(h, "sip_pass", s_cfg.sip_pass);
    err |= nvs_set_str(h, "caller_id", s_cfg.caller_id);
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "c%d_name", i);
        err |= nvs_set_str(h, k, s_cfg.contacts[i].name);
        snprintf(k, sizeof(k), "c%d_num", i);
        err |= nvs_set_str(h, k, s_cfg.contacts[i].number);
    }
    err |= nvs_set_u8(h, "provisioned", 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    s_cfg.provisioned = err == ESP_OK && s_cfg.contacts[0].number[0];
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving Twilio config failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    lock();
    mbedtls_platform_zeroize(&s_cfg, sizeof(s_cfg));
    unlock();
    ESP_LOGW(TAG, "factory reset: config erased (%s)", esp_err_to_name(err));
    return err;
}

#include "svc_wifi.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "app_events.h"

static const char *TAG = "svc_wifi";

/* ---- 私有状态 ---- */
typedef struct {
    bool            initialized;
    bool            running;
    esp_netif_t    *netif;
    int             retry_cnt;
} svc_wifi_ctx_t;

static svc_wifi_ctx_t s_ctx;

/* ---- WiFi / IP 事件处理 ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ctx.retry_cnt++;
        if (s_ctx.retry_cnt < CONFIG_SVC_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "retry connect (%d/%d)", s_ctx.retry_cnt, CONFIG_SVC_WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "connect failed after %d retries", s_ctx.retry_cnt);
            esp_event_post(APP_WIFI_EVENT, WIFI_DISCONNECTED, NULL, 0, 0);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_ctx.retry_cnt = 0;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        /* 路由器下发的 DNS 代理时好时坏，改用公共 DNS（主:阿里 备:114），
         * 避免偶发 getaddrinfo EAI_NONAME 导致云端请求全部失败 */
        esp_netif_dns_info_t dns;
        esp_netif_str_to_ip4("223.5.5.5", &dns.ip.u_addr.ip4);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_ctx.netif, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_str_to_ip4("114.114.114.114", &dns.ip.u_addr.ip4);
        esp_netif_set_dns_info(s_ctx.netif, ESP_NETIF_DNS_BACKUP, &dns);
        esp_event_post(APP_WIFI_EVENT, WIFI_CONNECTED, NULL, 0, 0);
        esp_event_post(APP_WIFI_EVENT, WIFI_GOT_IP, event, sizeof(*event), 0);
    }
}

esp_err_t svc_wifi_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    s_ctx.netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_ctx.netif != NULL, ESP_FAIL, TAG, "create sta netif failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    /* SSID / password 拷贝到配置 */
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_SVC_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    wifi_cfg.sta.ssid[sizeof(wifi_cfg.sta.ssid) - 1] = '\0';
    strncpy((char *)wifi_cfg.sta.password, CONFIG_SVC_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.password[sizeof(wifi_cfg.sta.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* 注册系统 WiFi/IP 事件 */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (ssid=%s)", CONFIG_SVC_WIFI_SSID);
    return ESP_OK;
}

esp_err_t svc_wifi_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.retry_cnt = 0;
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ctx.running = true;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t svc_wifi_stop(void)
{
    if (s_ctx.running) {
        esp_wifi_stop();
        s_ctx.running = false;
    }
    return ESP_OK;
}

esp_err_t svc_wifi_deinit(void)
{
    /* TODO: 注销事件监听、esp_wifi_deinit、销毁 netif */
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

void svc_wifi_register_console_cmds(void)
{
    /* TODO: 注册 "wifi status" / "wifi reconnect" 命令 */
}

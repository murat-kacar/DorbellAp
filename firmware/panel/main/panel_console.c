#include "panel_state.h"
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "panel_console";

static int cmd_doorbell_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t ip = 0;
    bool connected = doorbell_net_get_sta_ipv4(&ip);
    int8_t rssi = -127;
    uint8_t ch = 0;
    uint8_t bssid[6] = {0};
    uint16_t disconn = 0;
    doorbell_net_get_sta_telemetry(&rssi, &ch, bssid, &disconn);

    uint32_t reg_count = 0;
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    for (int i = CONFIG_DOORBELL_PANEL_MIN_APT_NO; i <= CONFIG_DOORBELL_PANEL_MAX_APT_NO; i++) {
        if (g_apt_slots[i].nvs.registered) {
            reg_count++;
        }
    }
    char ssid[33];
    strncpy(ssid, g_panel_cfg.wifi_ssid, sizeof(ssid));
    ssid[32] = '\0';
    xSemaphoreGive(g_panel_mutex);

    printf("Panel Mode       : %d\n", (int)g_panel_mode);
    printf("Configured SSID  : %s\n", ssid[0] ? ssid : "(none)");
    printf("STA Connected    : %s\n", connected ? "yes" : "no");
    printf("STA IPv4         : %lu.%lu.%lu.%lu\n",
           (unsigned long)((ip >> 24) & 0xFFU),
           (unsigned long)((ip >> 16) & 0xFFU),
           (unsigned long)((ip >> 8) & 0xFFU),
           (unsigned long)(ip & 0xFFU));
    printf("AP Channel/RSSI  : ch %u, %d dBm (disconnects=%u)\n",
           (unsigned)ch, (int)rssi, (unsigned)disconn);
    printf("Registered Apts  : %lu\n", (unsigned long)reg_count);
    printf("Last Restart     : %s (%u)\n",
           doorbell_reset_reason_str((uint8_t)esp_reset_reason()),
           (unsigned)esp_reset_reason());
    printf("Uptime           : %lld s\n", (long long)(esp_timer_get_time() / 1000000LL));
    printf("Free Heap        : %lu B (min %lu B)\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());
    return 0;
}

static int cmd_doorbell_wifi(int argc, char **argv)
{
    /* The REPL adds every line to the in-RAM history before running it; this line may contain the admin code and the
     * building passphrase, so drop the whole history first - also when the command is malformed (I-25) */
    linenoiseHistoryFree();

    if (argc < 3 || argc > 4) {
        printf("Usage: doorbell_wifi <admin_code> <ssid> [password]  (quote an SSID that contains spaces)\n");
        return 1;
    }

    const char *admin_candidate = argv[1];
    const char *new_ssid = argv[2];
    const char *new_pass = (argc >= 4) ? argv[3] : "";

    /* Single-Radio Rule (I-31): never re-target the STA while pairing or the setup portal owns the radio */
    if (g_panel_mode != MODE_IDLE) {
        printf("Error: panel is busy (pairing, portal or reset in progress); try again when idle.\n");
        return 1;
    }

    /* Syntax before authentication, as on the keypad (I-20): a malformed request is neither a wrong code nor a
     * successful login, so it must not touch the shared lockout counter */
    size_t pass_len = strlen(new_pass);
    if (strlen(new_ssid) == 0 || strlen(new_ssid) > 32 || (pass_len != 0 && (pass_len < 8 || pass_len > 63))) {
        printf("Error: invalid SSID or passphrase length.\n");
        return 1;
    }

    /* Authenticated console command (I-14, I-24): constant-time verify & shared lockout */
    if (!panel_verify_admin_code_ct(admin_candidate)) {
        printf("Error: authentication failed or panel is in lockout.\n");
        return 1;
    }

    /* Commit to NVS first, then to RAM: a failed save must leave the active credentials unchanged */
    xSemaphoreTake(g_panel_mutex, portMAX_DELAY);
    if (g_panel_mode != MODE_IDLE) {
        xSemaphoreGive(g_panel_mutex);
        printf("Error: panel became busy; credentials not changed.\n");
        return 1;
    }
    doorbell_panel_cfg_blob_t next = g_panel_cfg;
    memset(next.wifi_ssid, 0, sizeof(next.wifi_ssid));
    memset(next.wifi_pass, 0, sizeof(next.wifi_pass));
    strncpy(next.wifi_ssid, new_ssid, sizeof(next.wifi_ssid) - 1);
    strncpy(next.wifi_pass, new_pass, sizeof(next.wifi_pass) - 1);
    esp_err_t err = doorbell_storage_panel_save_cfg(&next);
    if (err == ESP_OK) {
        g_panel_cfg = next;
    }
    memset(&next, 0, sizeof(next));
    xSemaphoreGive(g_panel_mutex);

    if (err != ESP_OK) {
        printf("Error: failed to save Wi-Fi credentials to NVS (%s).\n", esp_err_to_name(err));
        return 1;
    }

    /* Never log or echo the passphrase or admin code (I-25) */
    ESP_LOGI(TAG, "Updated building Wi-Fi SSID to '%s' via authenticated UART console", new_ssid);
    doorbell_net_connect_sta(new_ssid, new_pass);
    printf("Wi-Fi credentials saved for SSID '%s'. Connecting...\n", new_ssid);
    return 0;
}

esp_err_t panel_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "panel> ";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Console UART REPL init skipped: %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t status_cmd = {
        .command = "doorbell_status",
        .help = "Display non-sensitive panel status and network telemetry",
        .hint = NULL,
        .func = &cmd_doorbell_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    const esp_console_cmd_t wifi_cmd = {
        .command = "doorbell_wifi",
        .help = "Update building Wi-Fi credentials: doorbell_wifi <admin_code> <ssid> [password]",
        .hint = "<admin_code> <ssid> [password]",
        .func = &cmd_doorbell_wifi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    return esp_console_start_repl(repl);
}


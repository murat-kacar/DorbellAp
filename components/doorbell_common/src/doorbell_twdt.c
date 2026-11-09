#include "doorbell_twdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "doorbell_twdt";

static esp_task_wdt_user_handle_t s_esp_timer_user = NULL;
static esp_task_wdt_user_handle_t s_rtos_timer_user = NULL;
static esp_timer_handle_t s_esp_timer_feed = NULL;
static TimerHandle_t s_rtos_timer_feed = NULL;

static void esp_timer_feed_cb(void *arg)
{
    (void)arg;
    esp_task_wdt_reset_user(s_esp_timer_user);
}

static void rtos_timer_feed_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_task_wdt_reset_user(s_rtos_timer_user);
}

esp_err_t doorbell_twdt_register_timer_pipelines(const char *esp_timer_user_name, const char *rtos_timer_user_name)
{
    esp_err_t err = esp_task_wdt_add_user(esp_timer_user_name, &s_esp_timer_user);
    if (err != ESP_OK) {
        return err;
    }
    const esp_timer_create_args_t args = {
        .callback = &esp_timer_feed_cb,
        .name = "twdt_feed",
    };
    err = esp_timer_create(&args, &s_esp_timer_feed);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_esp_timer_feed, 1000000ULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_task_wdt_add_user(rtos_timer_user_name, &s_rtos_timer_user);
    if (err != ESP_OK) {
        return err;
    }
    s_rtos_timer_feed = xTimerCreate("twdt_feed", pdMS_TO_TICKS(1000), pdTRUE, NULL, rtos_timer_feed_cb);
    if (!s_rtos_timer_feed || xTimerStart(s_rtos_timer_feed, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start FreeRTOS timer TWDT feed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

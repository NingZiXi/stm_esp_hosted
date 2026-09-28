#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

/* ESP-Hosted CP v3.0.9 provides this entry point without a public CP header. */
extern esp_err_t esp_hosted_init(void);

static const char *TAG = "esp32c3_cp";

void app_main(void)
{
    esp_err_t status = nvs_flash_init();
    if (status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        status = nvs_flash_init();
    }
    ESP_ERROR_CHECK(status);
    status = esp_event_loop_create_default();
    if (status != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(status);
    }
    /* Start RPC only after Wi-Fi storage and the event loop are ready. */
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_LOGI(TAG, "ESP-Hosted coprocessor started (SPI Full-Duplex)");
}

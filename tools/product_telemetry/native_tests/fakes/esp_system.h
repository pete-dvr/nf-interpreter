#pragma once
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_BROWNOUT };
esp_reset_reason_t esp_reset_reason();

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "ADB_HOST";

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            ESP_LOGI(TAG, "新设备接入！地址=%d", event_msg->new_dev.address);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGI(TAG, "设备拔出");
            break;
        default:
            break;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "启动 ESP32-S3 USB Host ADB 客户端");

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 2, NULL, 0);

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    usb_host_client_handle_t client_hdl;
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_hdl));

    ESP_LOGI(TAG, "等待手机插入...");

    while (1) {
        usb_host_client_handle_events(client_hdl, portMAX_DELAY);
    }
}

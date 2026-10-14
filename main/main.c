#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "ADB_HOST";

static usb_host_client_handle_t client_hdl;

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static void explore_config(usb_device_handle_t dev_hdl) {
    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) != ESP_OK) {
        ESP_LOGE(TAG, "获取配置描述符失败");
        return;
    }

    int offset = 0;
    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)((uint8_t *)config_desc + offset);
        if (desc->bLength == 0) break;

        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            ESP_LOGI(TAG, "接口 num=%d Class=%02X Sub=%02X Proto=%02X 端点数=%d",
                     intf->bInterfaceNumber, intf->bInterfaceClass,
                     intf->bInterfaceSubClass, intf->bInterfaceProtocol,
                     intf->bNumEndpoints);
            if (intf->bInterfaceClass == 0xFF) {
                ESP_LOGI(TAG, ">>> 找到 Class=FF 接口（可能是 ADB）");
            }
        }
        offset += desc->bLength;
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV: {
            ESP_LOGI(TAG, "新设备接入！地址=%d", event_msg->new_dev.address);
            usb_device_handle_t dev_hdl;
            if (usb_host_device_open(client_hdl, event_msg->new_dev.address, &dev_hdl) != ESP_OK) {
                ESP_LOGE(TAG, "打开设备失败");
                break;
            }
            const usb_device_desc_t *dev_desc;
            usb_host_get_device_descriptor(dev_hdl, &dev_desc);
            ESP_LOGI(TAG, "设备 VID=%04X PID=%04X",
                     dev_desc->idVendor, dev_desc->idProduct);
            explore_config(dev_hdl);
            break;
        }
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
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_hdl));

    ESP_LOGI(TAG, "等待手机插入...");

    while (1) {
        usb_host_client_handle_events(client_hdl, portMAX_DELAY);
    }
}

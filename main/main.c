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
        uint32_t f;
        usb_host_lib_handle_events(portMAX_DELAY, &f);
    }
}

static void explore(usb_device_handle_t dev) {
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return;
    int off = 0;
    while (off < cfg->wTotalLength) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t*)((uint8_t*)cfg + off);
        if (d->bLength == 0) break;
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t*)d;
            ESP_LOGI(TAG, "IF num=%d cls=%02X sub=%02X proto=%02X",
                     (int)i->bInterfaceNumber, (unsigned)i->bInterfaceClass,
                     (unsigned)i->bInterfaceSubClass, (unsigned)i->bInterfaceProtocol);
        }
        off += d->bLength;
    }
}

static void client_cb(const usb_host_client_event_msg_t *m, void *a) {
    switch (m->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV: {
            ESP_LOGI(TAG, "NEW DEV addr=%d", (int)m->new_dev.address);
            usb_device_handle_t dev;
            if (usb_host_device_open(client_hdl, m->new_dev.address, &dev) == ESP_OK) {
                const usb_device_desc_t *dd;
                usb_host_get_device_descriptor(dev, &dd);
                ESP_LOGI(TAG, "VID=%04X PID=%04X", (unsigned)dd->idVendor, (unsigned)dd->idProduct);
                explore(dev);
            }
            break;
        }
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGI(TAG, "DEV GONE");
            break;
        default: break;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "start usb host");
    usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&hc));
    xTaskCreatePinnedToCore(usb_lib_task, "usb", 4096, NULL, 2, NULL, 0);
    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &client_hdl));
    ESP_LOGI(TAG, "waiting for phone...");
    while (1) usb_host_client_handle_events(client_hdl, portMAX_DELAY);
}

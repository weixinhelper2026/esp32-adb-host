#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "ADB_HOST";

#define ADB_CLASS     0xFF
#define ADB_SUBCLASS  0x42
#define ADB_PROTOCOL  0x01
#define ADB_VERSION   0x01000000
#define ADB_MAX_DATA  4096

typedef struct {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t magic;
} __attribute__((packed)) adb_msg_t;

static usb_host_client_handle_t client_hdl;
static SemaphoreHandle_t device_ready_sem;
static usb_device_handle_t phone_dev = NULL;
static uint8_t ep_in_addr = 0;
static uint8_t ep_out_addr = 0;
static int ep_in_mps = 64;
static int ep_out_mps = 64;
static uint8_t intf_num = 0;

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static void usb_client_task(void *arg) {
    while (1) {
        usb_host_client_handle_events(client_hdl, portMAX_DELAY);
    }
}

static uint32_t adb_crc(const uint8_t *d, uint32_t len) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < len; i++) s += d[i];
    return s;
}

static bool find_adb_interface(usb_device_handle_t dev) {
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return false;

    int offset = 0;
    bool in_adb = false;
    while (offset < cfg->wTotalLength) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)((uint8_t *)cfg + offset);
        if (d->bLength == 0) break;

        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
            if (intf->bInterfaceClass == ADB_CLASS &&
                intf->bInterfaceSubClass == ADB_SUBCLASS &&
                intf->bInterfaceProtocol == ADB_PROTOCOL) {
                in_adb = true;
                intf_num = intf->bInterfaceNumber;
                ESP_LOGI(TAG, "找到ADB接口 num=%d", (int)intf_num);
            } else {
                in_adb = false;
            }
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_adb) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if ((ep->bmAttributes & 0x03) == 0x02) {
                if (ep->bEndpointAddress & 0x80) {
                    ep_in_addr = ep->bEndpointAddress;
                    ep_in_mps = ep->wMaxPacketSize;
                    ESP_LOGI(TAG, "IN 端点 0x%02X mps=%d", (int)ep_in_addr, ep_in_mps);
                } else {
                    ep_out_addr = ep->bEndpointAddress;
                    ep_out_mps = ep->wMaxPacketSize;
                    ESP_LOGI(TAG, "OUT端点 0x%02X mps=%d", (int)ep_out_addr, ep_out_mps);
                }
            }
        }
        offset += d->bLength;
    }
    return (ep_in_addr != 0 && ep_out_addr != 0);
}

static esp_err_t adb_bulk_out(const uint8_t *data, uint32_t len) {
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > (uint32_t)ep_out_mps) chunk = ep_out_mps;

        usb_transfer_t *t;
        if (usb_host_transfer_alloc(chunk, 0, &t) != ESP_OK) return ESP_FAIL;
        t->device_handle = phone_dev;
        t->bEndpointAddress = ep_out_addr;
        t->callback = NULL;
        t->context = NULL;
        t->num_bytes = chunk;
        memcpy(t->data_buffer, data + sent, chunk);

        esp_err_t err = usb_host_transfer_submit(t);
        if (err != ESP_OK) {
            usb_host_transfer_free(t);
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        usb_host_transfer_free(t);
        sent += chunk;
    }
    return ESP_OK;
}

static int adb_bulk_in(uint8_t *data, uint32_t max_len) {
    uint32_t chunk = max_len > (uint32_t)ep_in_mps ? ep_in_mps : max_len;
    usb_transfer_t *t;
    if (usb_host_transfer_alloc(chunk, 0, &t) != ESP_OK) return -1;
    t->device_handle = phone_dev;
    t->bEndpointAddress = ep_in_addr;
    t->callback = NULL;
    t->context = NULL;
    t->num_bytes = chunk;

    esp_err_t err = usb_host_transfer_submit(t);
    if (err != ESP_OK) {
        usb_host_transfer_free(t);
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    int n = t->actual_num_bytes;
    if (n > 0) memcpy(data, t->data_buffer, n);
    usb_host_transfer_free(t);
    return n;
}

static void adb_send_cmd(const char *cmd, uint32_t a0, uint32_t a1, const void *data, uint32_t dlen) {
    adb_msg_t m;
    memcpy(&m.command, cmd, 4);
    m.arg0 = a0;
    m.arg1 = a1;
    m.data_length = dlen;
    m.data_crc32 = (data && dlen) ? adb_crc((const uint8_t *)data, dlen) : 0;
    m.magic = m.command ^ 0xFFFFFFFF;

    uint32_t total = sizeof(adb_msg_t) + dlen;
    uint8_t *buf = malloc(total);
    memcpy(buf, &m, sizeof(adb_msg_t));
    if (data && dlen) memcpy(buf + sizeof(adb_msg_t), data, dlen);
    adb_bulk_out(buf, total);
    free(buf);
    ESP_LOGI(TAG, "已发送 %s (%d 字节)", cmd, (int)dlen);
}

static void adb_run_protocol(void) {
    ESP_LOGI(TAG, "开始ADB握手");

    const char *banner = "host::features=shell_v2,cmd,abb_exec";
    adb_send_cmd("CNXN", ADB_VERSION, ADB_MAX_DATA, banner, strlen(banner) + 1);

    uint8_t buf[256];
    int n = adb_bulk_in(buf, sizeof(buf));
    if (n < 24) {
        ESP_LOGW(TAG, "没收到有效响应 (n=%d)", n);
        return;
    }

    adb_msg_t *resp = (adb_msg_t *)buf;
    char cmd[5] = {0};
    memcpy(cmd, &resp->command, 4);
    ESP_LOGI(TAG, "收到响应: %s arg0=%08X arg1=%08X len=%u", cmd,
             (unsigned int)resp->arg0, (unsigned int)resp->arg1, (unsigned int)resp->data_length);

    if (memcmp(cmd, "CNXN", 4) == 0) {
        ESP_LOGI(TAG, "握手成功，发送 OPEN tcpip:5555");
        const char *dest = "tcpip:5555";
        adb_send_cmd("OPEN", 1, 0, dest, strlen(dest) + 1);
        n = adb_bulk_in(buf, sizeof(buf));
        if (n >= 24) {
            memcpy(cmd, buf, 4);
            ESP_LOGI(TAG, "OPEN响应: %s", cmd);
        }
    } else if (memcmp(cmd, "AUTH", 4) == 0) {
        ESP_LOGW(TAG, "手机要求AUTH认证，需要RSA签名（下一步）");
    } else {
        ESP_LOGW(TAG, "未知响应");
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV: {
            ESP_LOGI(TAG, "新设备接入！地址=%d", (int)msg->new_dev.address);
            if (usb_host_device_open(client_hdl, msg->new_dev.address, &phone_dev) != ESP_OK) {
                ESP_LOGE(TAG, "打开设备失败");
                break;
            }
            const usb_device_desc_t *dd;
            usb_host_get_device_descriptor(phone_dev, &dd);
            ESP_LOGI(TAG, "VID=%04X PID=%04X", (unsigned int)dd->idVendor, (unsigned int)dd->idProduct);

            ep_in_addr = 0;
            ep_out_addr = 0;

            if (find_adb_interface(phone_dev)) {
                ESP_LOGI(TAG, "ADB接口就绪");
                xSemaphoreGive(device_ready_sem);
            } else {
                ESP_LOGW(TAG, "没找到ADB接口（手机未开启USB调试？）");
            }
            break;
        }
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGI(TAG, "设备拔出");
            phone_dev = NULL;
            break;
        default:
            break;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "启动 ESP32-S3 USB Host ADB 客户端");

    device_ready_sem = xSemaphoreCreateBinary();

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 2, NULL, 0);

    usb_host_client_config_t cli_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cli_cfg, &client_hdl));

    xTaskCreatePinnedToCore(usb_client_task, "usb_client", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "等待手机插入...");

    while (1) {
        if (xSemaphoreTake(device_ready_sem, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(800));
            adb_run_protocol();
        }
    }
}

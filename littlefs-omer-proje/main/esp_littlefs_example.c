#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "http_parser.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/spi_master.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_littlefs.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define PIN_NUM_MISO 38
#define PIN_NUM_MOSI 36
#define PIN_NUM_CLK 37
#define PIN_NUM_CS 7

static const char *TAG = "webserver";
#define MOUNT_PATH "/littlefs"
static const char *OTA_SERVER_IP = "192.168.4.1";
static const char *OTA_SERVER_PORT = "80";

void print_file_info(const char *filepath);
esp_err_t perform_ota_update(const char *file_path);

static const char *html_content =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<style>"
".button {"
"    border: none;"
"    color: white;"
"    padding: 15px 32px;"
"    text-align: center;"
"    text-decoration: none;"
"    display: inline-block;"
"    font-size: 16px;"
"    margin: 4px 2px;"
"    cursor: pointer;"
"    background-color: #008CBA;"
"}"
".progress {"
"    width: 100%;"
"    background-color: #f1f1f1;"
"}"
".bar {"
"    width: 0%;"
"    height: 30px;"
"    background-color: #4CAF50;"
"    text-align: center;"
"    line-height: 30px;"
"    color: white;"
"}"
"</style>"
"</head>"
"<body>"
"<h1>OTA Button</h1>"
"<p>Select a .bin file from your computer and click the button to transfer it to OTA:</p>"
"<input type='file' id='fileInput'>"
"<button class=\"button\" onclick=\"transferBinFile()\">OTA</button>"
"<div class='progress'>"
"    <div class='bar' id='progressBar'></div>"
"</div>"
"<script>"
"function arrayBufferToHex(arrayBuffer) {"
"    var view = new DataView(arrayBuffer);"
"    var hex = '';"
"    for (var i = 0; i < view.byteLength; i++) {"
"        var val = view.getUint8(i).toString(16).toUpperCase();"
"        hex += (val.length === 1 ? '0' + val : val);"
"    }"
"    return hex;"
"}"
""
"function transferBinFile() {"
"    var fileInput = document.getElementById('fileInput');"
"    var file = fileInput.files[0];"
"    if (!file) {"
"        alert('Please select a file first');"
"        return;"
"    }"
""
"    var reader = new FileReader();"
"    reader.onload = function(event) {"
"        var fileContent = event.target.result;"
"        var hexContent = arrayBufferToHex(fileContent);"
"        console.log('File content (HEX):', hexContent);"
""
"        var xhr = new XMLHttpRequest();"
"        xhr.open('POST', '/upload', true);"
"        xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
"        xhr.upload.onprogress = function(e) {"
"            var progressBar = document.getElementById('progressBar');"
"            if (e.lengthComputable) {"
"                var percentComplete = (e.loaded / e.total) * 100;"
"                progressBar.style.width = percentComplete + '%';"
"            }"
"        };"
"        xhr.onload = function() {"
"            alert(xhr.responseText);"
"        };"
"        xhr.send(fileContent);"
"    };"
"    reader.readAsArrayBuffer(file);"
"}"
"</script>"
"</body>"
"</html>";

static esp_err_t html_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, html_content, strlen(html_content));
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    const char *filename = MOUNT_PATH "/upload";
    FILE *fd = fopen(filename, "wb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }

    char buffer[1024] = {0};
    int remaining = req->content_len;

    while (remaining > 0)
    {
        int received = httpd_req_recv(req, buffer, MIN(remaining, sizeof(buffer)));
        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            fclose(fd);
            return ESP_FAIL;
        }

        if (received && (received != fwrite(buffer, 1, received, fd)))
        {
            fclose(fd);
            ESP_LOGE(TAG, "File write failed");
            return ESP_FAIL;
        }
        remaining -= received;
        memset(buffer, 0, 1024);
    }

    fclose(fd);
    ESP_LOGI(TAG, "File transferred successfully to LittleFS");

    print_file_info(filename);

    httpd_resp_sendstr(req, "File transferred successfully to LittleFS");

    perform_ota_update(filename);

    return ESP_OK;
}

static const httpd_uri_t html = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = html_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t upload = {
    .uri = "/upload",
    .method = HTTP_POST,
    .handler = upload_post_handler,
    .user_ctx = NULL
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &html);
        httpd_register_uri_handler(server, &upload);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    httpd_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server)
    {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL)
    {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}

#define EXAMPLE_ESP_WIFI_SSID "esp32omer12"
#define EXAMPLE_ESP_WIFI_PASS "yourPASSWORD"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_MAX_STA_CONN 4

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

void print_file_info(const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) == 0)
    {
        ESP_LOGI(TAG, "File size: %ld bytes", st.st_size);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to get file size");
    }
}

esp_err_t perform_ota_update(const char *file_path)
{
    ESP_LOGI(TAG, "Starting OTA update...");

    FILE *fd = fopen(file_path, "rb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to open file for OTA update");
        return ESP_FAIL;
    }

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition)
    {
        ESP_LOGE(TAG, "Failed to get update partition");
        fclose(fd);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle));

    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fd)) > 0)
    {
        ESP_ERROR_CHECK(esp_ota_write(update_handle, buffer, bytes_read));
    }

    fclose(fd);

    ESP_ERROR_CHECK(esp_ota_end(update_handle));
    ESP_ERROR_CHECK(esp_ota_set_boot_partition(update_partition));

    ESP_LOGI(TAG, "OTA update completed. Rebooting...");
    esp_restart();

    return ESP_OK;
}



void init_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}


void app_main(void)
{
	  init_littlefs();
	  
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_softap();

    esp_vfs_littlefs_conf_t conf = {
        .base_path = MOUNT_PATH,
        .partition_label = NULL,
        .format_if_mount_failed = true,
        .dont_mount = false
    };

    ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount or format LittleFS");
        return;
    }

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &connect_handler, &server, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server, NULL));

    server = start_webserver();
}

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#define WIFI_SSID "rogo"
#define WIFI_PASSWORD "123456789"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define TAG_WIFI "[WIFI STATION]"
#define TAG_HTTPSERVER "[HTTPSERVER]"

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
        {
            if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
            {
                esp_wifi_connect();
            }
            else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
            {
                if (s_retry_num < 3)
                {
                    esp_wifi_connect();
                    s_retry_num++;
                    ESP_LOGI(TAG_WIFI, "retry to connect to the AP");
                }
                else
                {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                ESP_LOGI(TAG_WIFI, "connect to the AP fail");
            }
            else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
            {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG_WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                s_retry_num = 0;
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }


class WIFI_Handler {

    public:

        WIFI_Handler() {
            s_retry_num = 0;
        }

        void init_sta(void) {

            s_wifi_event_group = xEventGroupCreate();

            ESP_ERROR_CHECK(esp_netif_init());

            ESP_ERROR_CHECK(esp_event_loop_create_default());
            esp_netif_create_default_wifi_sta();

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));

            esp_event_handler_instance_t instance_any_id;
            esp_event_handler_instance_t instance_got_ip;
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &event_handler,
                                                                NULL,
                                                                &instance_any_id));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                                IP_EVENT_STA_GOT_IP,
                                                                &event_handler,
                                                                NULL,
                                                                &instance_got_ip));

            wifi_config_t wifi_config = {
                .sta = {
                    .ssid = WIFI_SSID,
                    .password = WIFI_PASSWORD,
                    .threshold = {
                        .authmode = WIFI_AUTH_WPA2_PSK
                    }
                },
            };

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());

            ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");

            /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
            * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE,
                                                pdFALSE,
                                                portMAX_DELAY);

            /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
            * happened. */
            if (bits & WIFI_CONNECTED_BIT)
            {
                ESP_LOGI(TAG_WIFI, "connected to ap SSID:%s password:%s",
                        WIFI_SSID, WIFI_PASSWORD);
            }
            else if (bits & WIFI_FAIL_BIT)
            {
                ESP_LOGI(TAG_WIFI, "Failed to connect to SSID:%s, password:%s",
                        WIFI_SSID, WIFI_PASSWORD);
            }
            else
            {
                ESP_LOGE(TAG_WIFI, "UNEXPECTED EVENT");
            }

            /* The event will not be processed after unregister */
            ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
            ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
            vEventGroupDelete(s_wifi_event_group);
        }
}; 


class HTTPServer
{
    public:
        
        httpd_handle_t start(void) {

            httpd_handle_t server = NULL;
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();

            // Start the server
            if (httpd_start(&server, &config) == ESP_OK) {
                // Register the POST endpoint
                httpd_uri_t post_uri = {
                    .uri = "/post",
                    .method = HTTP_POST,
                    .handler = post_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &post_uri);

                // Register the GET endpoint
                httpd_uri_t get_uri = {
                    .uri = "/get",
                    .method = HTTP_GET,
                    .handler = get_handler,
                    .user_ctx = NULL
                };
                httpd_register_uri_handler(server, &get_uri);

                return server;
            }

            ESP_LOGI(TAG_HTTPSERVER, "Error starting server!");
            return NULL;
        }

        static esp_err_t post_handler(httpd_req_t *req)
        {
            char content[200];
            int content_length = httpd_req_get_hdr_value_len(req, "Content-Length") + 1;
            if (content_length > sizeof(content)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
                return ESP_FAIL;
            }
            httpd_req_recv(req, content, content_length);
            content[content_length] = '\0';
            httpd_resp_sendstr(req, "POST request received");
            ESP_LOGI(TAG_HTTPSERVER, "Received data: %s", content);

            nvs_handle_t nvs_handler;
            nvs_open("storage", NVS_READWRITE, &nvs_handler);

            nvs_set_str(nvs_handler, "0x123", content);
            nvs_commit(nvs_handler);

            return ESP_OK;
        }

        static esp_err_t get_handler(httpd_req_t *req)
        {
            nvs_handle_t nvs_handler;
            nvs_open("storage", NVS_READONLY, &nvs_handler);
            
            size_t required_size = 0;
            nvs_get_str(nvs_handler, "0x123", NULL, &required_size);
            char *data = static_cast<char*>(malloc(required_size));
            nvs_get_str(nvs_handler, "0x123", data, &required_size);

            httpd_resp_send(req, data, sizeof(data) - 1);
            return ESP_OK;
        }

        void stop(httpd_handle_t server)
        {
            if (server) {
                httpd_stop(server);
            }
        }

};

extern "C" void app_main() {

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG_WIFI, "Lauching wifi connection");
    WIFI_Handler wifi = WIFI_Handler();
    wifi.init_sta();

    HTTPServer serverObj = HTTPServer();
    httpd_handle_t server = serverObj.start();
    if (server == NULL) {
        ESP_LOGI(TAG_HTTPSERVER, "Failed to start HTTP server");
        return;
    }
}


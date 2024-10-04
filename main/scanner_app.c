#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gattc_api.h"

#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "esp_http_client.h"

#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_netif.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define DEBUG_PRINT "SCANNER_PRINT"
#define HTTP_PRINT "HTTP"
#define WIFI_DEBUG_PRINT "WIFI_STA"

#define NAME_WIFI      CONFIG_ESP_WIFI_SSID
#define PASSWORD_WIFI  CONFIG_ESP_WIFI_PASSWORD
#define SERVER_ADDR      CONFIG_ESP_IP_ADDRESS

#define UUID_LENGTH 37
#define SCANNING_DURATION 5

// STRUCTS -------------------------------------------------------


// Device structure
struct Device {
   char name[50];
   char address[18];
   int rssi;
   bool in_range;
   int services_count;
   int chars_count;
   char services[20][UUID_LENGTH];
   char chars[20][UUID_LENGTH];
};

// Scanning parameters
static esp_ble_scan_params_t scanning_parameters = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

// ---------------------------------------------------------------

// VARIABLES -----------------------------------------------------

struct Device devices[10];

int connected_device_index = -1;
int devices_count = 0;

static bool connected_to_device = false;
bool connected_to_wifi = false;
static bool is_discovering = false;

char* device_to_discover = "";

static void handle_gap_events(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *gap_cb_param);
static void handle_gatt_events(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_interface_type, esp_ble_gattc_cb_param_t *gattc_cb_param);

static esp_gattc_cb_t gattc_cb = handle_gatt_events;
static uint16_t global_gattc_interface_type = ESP_GATT_IF_NONE;

// HELPER FUNCTIONS ---------------------------------------------------------------------------

// Convert UUID to String
static char *get_str_from_uuid(esp_bt_uuid_t uuid, char *str, size_t size) {

    if (uuid.len == 2 && size >= 5) {
        sprintf(str, "%04x", uuid.uuid.uuid16);
    } else if (uuid.len == 4 && size >= 9) {
        sprintf(str, "%08x", uuid.uuid.uuid32);
    } else if (uuid.len == 16 && size >= 37) {
        uint8_t *p = uuid.uuid.uuid128;
        sprintf(str, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8],
                p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    } else {
        return NULL;
    }

    return str;
}

// Extract String Address 
static void *get_string_from_raw_addr(esp_bd_addr_t peripheral_addr, char *str){
    if (peripheral_addr != NULL) {
        sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", peripheral_addr[0], peripheral_addr[1], peripheral_addr[2], peripheral_addr[3], peripheral_addr[4], peripheral_addr[5]);
    }
    return;
}

char* concat(const char *str1, const char *str2) {
    char *str3 = malloc(strlen(str1) + strlen(str2) + 1);
    strcpy(str3, str1);
    strcat(str3, str2);
    return str3;
}

// ------------------------------------------------------------------------------------------------

// HTTP -------------------------------------------------------------------------------------------

// Handling HTTP Events
esp_err_t handle_http_events(esp_http_client_event_t *http_event) {
    switch(http_event->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            break;
        case HTTP_EVENT_ON_FINISH:
            break;

        // If data was received
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(http_event->client)) {
                device_to_discover = strdup((char*)http_event->data);
                device_to_discover[http_event->data_len] = '\0';
                is_discovering = true;

                ESP_LOGE(HTTP_PRINT, "Got discovery request for device: %s", device_to_discover);
            }
            break;
        case HTTP_EVENT_HEADER_SENT:
            break;
        case HTTP_EVENT_ON_HEADER:
            break;
        case HTTP_EVENT_ERROR:
            break;
        case HTTP_EVENT_DISCONNECTED:
            break;
    }
    return ESP_OK;
}

// Send data to server
static void send_http_request_with_url(int device_index) {

    char *server_addr = SERVER_ADDR;

    char *http_str = "http://";
    http_str = concat(http_str, server_addr);
    http_str = concat(http_str, "/RESTServerScanner-1.0-SNAPSHOT/api/scanner?address=");

    http_str = concat(http_str, devices[device_index].address);

    char device_name[50];

    char *services = "";
    char *chars = "";

    int offset = 0;
    int index = 0;

    // Adding device name to http string
    if (devices[device_index].name != NULL) {

        http_str = concat(http_str, "&device=");

        // Filtering http string
        for(index = 0; devices[device_index].name[index] != '\0'; index++) {
            if(devices[device_index].name[index] == ' ') {
                device_name[index+offset] = '%';
                device_name[index+offset+1] = '2';
                device_name[index+offset+2] = '0';
                offset += 2;
            } else if(devices[device_index].name[index] == '+') {
                device_name[index+offset] = '%';
                device_name[index+offset+1] = '2';
                device_name[index+offset+2] = 'B';
                offset += 2;
            } else {
                device_name[index+offset] = devices[device_index].name[index];
            }
        }

        device_name[index+offset] = '\0';
        http_str = concat(http_str, device_name);
    }

    // Adding services to http string
    if ((strcmp((char*)devices[device_index].address, device_to_discover) == 0)  
                && devices[device_index].services_count > 0) {
        device_to_discover = "\0";
        is_discovering = false;
        services = concat(services, "&services=");
                    
        for (int i = 0; i < devices[device_index].services_count; i++) {
            services = concat(services, devices[device_index].services[i]);
                if (i != devices[device_index].services_count - 1) {
                    services = concat(services, ",");
                }
        }

        // Adding characteristics to http string
        if (devices[device_index].chars_count > 0) {
            chars = concat(chars, "&chars=");
            chars = concat(chars, devices[device_index].chars[0]);
            for (int i = 0; i < devices[device_index].chars_count; i++) {
                chars = concat(chars, devices[device_index].chars[i]);
                if  (i != devices[device_index].chars_count - 1) {
                    chars = concat(chars, ",");
                }

            }

        }

        http_str = concat(http_str, chars);
        http_str = concat(http_str, services);
    }

    // Adding RSSI value to http string
    http_str = concat(http_str, "&rssi=");

    char num_str[5];
    itoa(devices[device_index].rssi, num_str, 10);
    http_str = concat(http_str, num_str);

    ESP_LOGE(HTTP_PRINT, "SENDING DATA TO SERVER");

    esp_http_client_config_t http_client_config = {
        .url = http_str,
        .event_handler = handle_http_events,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t http_client_handle = esp_http_client_init(&http_client_config);

    // Sending data
    esp_err_t err = esp_http_client_perform(http_client_handle);

    esp_http_client_cleanup(http_client_handle);

}

// END HTTP ---------------------------------------------------------------------------------------

// WIFI -------------------------------------------------------------------------------------------

// Handling WiFi events
static void got_wifi_event(void* arg, esp_event_base_t wifi_event, int32_t wifi_event_num, void* wifi_raw_event) {

    // If WiFi scanning started or disconnected from Access Point
    if (wifi_event == WIFI_EVENT && (wifi_event_num == WIFI_EVENT_STA_START || WIFI_EVENT_STA_DISCONNECTED)) {
        ESP_LOGE(WIFI_DEBUG_PRINT, "Trying to connect to Wifi");

        connected_to_wifi = false;

        ESP_ERROR_CHECK(esp_wifi_connect());

    // If connected
    } else if (wifi_event == IP_EVENT && wifi_event_num == IP_EVENT_STA_GOT_IP) {

        // Getting IP
        ip_event_got_ip_t* ip_event = (ip_event_got_ip_t*) wifi_raw_event;
        ESP_LOGE(WIFI_DEBUG_PRINT, "CONNECTED TO WIFI");
        ESP_LOGI(WIFI_DEBUG_PRINT, "IP address:" IPSTR, IP2STR(&ip_event->ip_info.ip));

        connected_to_wifi = true;
    }
}

void connect_to_wifi(void) {
    // Initialize the underlying TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Initializing default WiFi event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Creating WiFi station
    esp_netif_create_default_wifi_sta();

    // Initializing WiFi configuration options
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

    // Initializing WiFi allocate resource for WiFi driver
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    esp_event_handler_instance_t id_event_instance;
    esp_event_handler_instance_t ip_event_instance;

    // Setting callbacks for WiFi events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &got_wifi_event, NULL, &id_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_wifi_event, NULL, &ip_event_instance));


    wifi_config_t wifi_config = {
        .sta = {
            .ssid = NAME_WIFI,
            .password = PASSWORD_WIFI,
	        .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // Setting operating mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Setting configuration of the station
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Starting WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ------------------------------------------------------------------------------------------------

// Print found devices
static void show_found_devices() {
    for(int i = 0; i < devices_count; i++) {

        if (devices[i].in_range == true) {

            // If connected, send information about device to server
            if (connected_to_wifi == true) {
                send_http_request_with_url(i);
            }

            if (devices[i].name != NULL) {
            ESP_LOGI("FOUND DEVICE", "(%s, %s, %d)", devices[i].address, devices[i].name, devices[i].rssi);
            } else {
                ESP_LOGI("FOUND DEVICE", "(%s, %d)", devices[i].address, devices[i].rssi);
            }

            devices[i].in_range = false;
        }
    }
}

// Adding new device to device structure
static int add_device(esp_ble_gap_cb_param_t *gap_cb_param) {
    struct Device new_device;

    uint8_t *peripheral_name = NULL;
    uint8_t peripheral_name_length = 0;
    
    // Getting device name
    peripheral_name = esp_ble_resolve_adv_data(gap_cb_param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &peripheral_name_length);

    if (peripheral_name != NULL) {
        snprintf(new_device.name, (int) peripheral_name_length+1, "%s", (char *) peripheral_name);
        //strcpy(new_device.name, device_name);
    } else {
        //char null_str[50] = "null";
        strcpy(new_device.name, "-");
    }

    get_string_from_raw_addr(gap_cb_param->scan_rst.bda, new_device.address);

    new_device.rssi = gap_cb_param->scan_rst.rssi;

    new_device.services_count = 0;
    new_device.chars_count = 0;
    new_device.in_range = true;

    if (devices_count >= 10) {
        printf("Connected device: %d", connected_device_index);
        memset(devices, 0, sizeof(devices));
        devices_count = 0;
    }

    devices[devices_count] = new_device;

    return devices_count++;
}

// Update name and RSSI value of the known device
static void update_device_info(int device_index, esp_ble_gap_cb_param_t *gap_cb_param) {
    devices[device_index].rssi = gap_cb_param->scan_rst.rssi;
    devices[device_index].in_range = true;

    if (strcmp((char *)devices[device_index].name, "-") == 0) {

        uint8_t *peripheral_name = NULL;
        uint8_t peripheral_name_length = 0;
        
        peripheral_name = esp_ble_resolve_adv_data(gap_cb_param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &peripheral_name_length);

        if (peripheral_name != NULL) {
            snprintf(devices[device_index].name, (int) peripheral_name_length+1, "%s", (char *) peripheral_name);
            //strcpy(new_device.name, device_name);
        } else {
            //char null_str[50] = "null";
            strcpy(devices[device_index].name, "-");
        }

    }

}

// Start services and characteristics extraction
static void discover_device(int current_device, esp_ble_gap_cb_param_t *gap_cb_param) {
    if (connected_to_device == false) {
        connected_to_device = true;
        connected_device_index = current_device;
        esp_ble_gap_stop_scanning();

        ESP_LOGI(DEBUG_PRINT, "Trying to connect to peripheral");

        // Connecting to device
        esp_ble_gattc_open(global_gattc_interface_type, gap_cb_param->scan_rst.bda, gap_cb_param->scan_rst.ble_addr_type, true);
    }
}

// Extracting characteristics
static void discover_characteristics(esp_ble_gattc_cb_param_t *gattc_cb_parameters, esp_gatt_if_t gattc_interface_type) {

    esp_gattc_char_elem_t *characteristic_element = NULL;

    char uuid_str[UUID_LENGTH];
    get_str_from_uuid(gattc_cb_parameters->search_res.srvc_id.uuid, devices[connected_device_index].services[devices[connected_device_index].services_count++], UUID_LENGTH);

    uint16_t found_chars = 0;

    // Getting number of characteristics
    esp_ble_gattc_get_attr_count(gattc_interface_type, gattc_cb_parameters->search_res.conn_id, ESP_GATT_DB_CHARACTERISTIC, gattc_cb_parameters->search_res.start_handle,
                                                                     gattc_cb_parameters->search_res.end_handle, 0, &found_chars);
    if (found_chars > 0){
        characteristic_element = (esp_gattc_char_elem_t *) malloc(sizeof(esp_gattc_char_elem_t) * found_chars);
        if (characteristic_element){

            ESP_LOGE(DEBUG_PRINT, "Finding Characteristics...");

            // Searching for characteristics
            esp_ble_gattc_get_all_char(gattc_interface_type, gattc_cb_parameters->search_res.conn_id, gattc_cb_parameters->search_res.start_handle, 
                                        gattc_cb_parameters->search_res.end_handle, characteristic_element, &found_chars, 0);

            if (found_chars > 0) {
                for (int i = 0; i < found_chars; i++) {

                    char char_uuid_str[UUID_LENGTH];
                    get_str_from_uuid(characteristic_element[i].uuid, devices[connected_device_index].chars[devices[connected_device_index].chars_count++], UUID_LENGTH);

                    ESP_LOGI(DEBUG_PRINT, "Characteristic UUID: %x", characteristic_element[i].uuid.uuid.uuid16);
                }
            } else {
                ESP_LOGE(DEBUG_PRINT, "Couldn't find any characteristics");
            }
        }
        free(characteristic_element);
    } else {
        ESP_LOGE(DEBUG_PRINT, "Couldn't find any characteristics");
    }

}

// Handling GATT events
static void handle_gatt_events(esp_gattc_cb_event_t gattc_cb_event, esp_gatt_if_t gattc_interface_type, esp_ble_gattc_cb_param_t *gattc_cb_param){

    esp_ble_gattc_cb_param_t *gattc_cb_parameters = (esp_ble_gattc_cb_param_t *)gattc_cb_param;

    switch (gattc_cb_event) {

    // When GATT client is registered
    case ESP_GATTC_REG_EVT:
        if (gattc_cb_param->reg.status == ESP_GATT_OK) {
            global_gattc_interface_type = gattc_interface_type;
        }
        // Setting scanning parameters
        esp_ble_gap_set_scan_params(&scanning_parameters);
        break;

    // Opening event
    case ESP_GATTC_OPEN_EVT:
        // If open failed
        if (gattc_cb_param->open.status != ESP_GATT_OK){
            ESP_LOGE(DEBUG_PRINT, "Failed to open device: %d", gattc_cb_parameters->open.status);
            strcpy(devices[connected_device_index].services[devices[connected_device_index].services_count++], "-");
            strcpy(devices[connected_device_index].chars[devices[connected_device_index].chars_count++], "-");
            break;
        }
        // if success
        ESP_LOGI(DEBUG_PRINT, "Successfully open device");
        break;

    // Servies discovering completed
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        // If failed
        if (gattc_cb_param->dis_srvc_cmpl.status != ESP_GATT_OK){
            ESP_LOGE(DEBUG_PRINT, "Service discovery failed: %d", gattc_cb_param->dis_srvc_cmpl.status);
            break;
        }
        // Geting services from local cache
        esp_ble_gattc_search_service(gattc_interface_type, gattc_cb_param->cfg_mtu.conn_id, NULL);
        break;

    // Disconnected from device
    case ESP_GATTC_DISCONNECT_EVT:
        connected_to_device = false;
        connected_device_index = -1;
        esp_ble_gap_start_scanning(SCANNING_DURATION);
        break;

    // Found service
    case ESP_GATTC_SEARCH_RES_EVT: {
        ESP_LOGI(DEBUG_PRINT, "Service found");

        char uuid_str[UUID_LENGTH];
        esp_bt_uuid_t u = gattc_cb_parameters->search_res.srvc_id.uuid;
        ESP_LOGI(DEBUG_PRINT, "UUID: %s", get_str_from_uuid(u, uuid_str, UUID_LENGTH));

        discover_characteristics(gattc_cb_parameters, gattc_interface_type);
        break;
    }
    // Services searching completed
    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGE(DEBUG_PRINT, "Services discovery complete");

        // Disconnecting from device
        esp_ble_gattc_close(gattc_interface_type, gattc_cb_parameters->search_res.conn_id);
         break;
    default:
        break;
    }
}


// Handling GAP events
static void handle_gap_events(esp_gap_ble_cb_event_t gap_cb_event, esp_ble_gap_cb_param_t *gattc_cb_param)
{
    uint8_t *peripheral_name = NULL;
    uint8_t peripheral_name_length = 0;
    char peripheral_address[18];

    switch (gap_cb_event) {

    // Scanning parameters set
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        esp_ble_gap_start_scanning(SCANNING_DURATION);
        break;
    }

    // Scanning started
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:

        // If failed
        if (gattc_cb_param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(DEBUG_PRINT, "Failed to start scanning: %x", gattc_cb_param->scan_start_cmpl.status);
            esp_ble_gap_start_scanning(SCANNING_DURATION);
            break;
        }
        ESP_LOGI(DEBUG_PRINT, "Scanning started");

        break;

    // Scanning stopped
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(DEBUG_PRINT, "Scanning stopped");
        break;

    // Got scanning result
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *gap_cb_param = (esp_ble_gap_cb_param_t *)gattc_cb_param;

        switch (gap_cb_param->scan_rst.search_evt) {

        // Got inquiry result for device
        case ESP_GAP_SEARCH_INQ_RES_EVT:
            peripheral_name = esp_ble_resolve_adv_data(gap_cb_param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &peripheral_name_length);
            get_string_from_raw_addr(gap_cb_param->scan_rst.bda, peripheral_address);

            int current_device = -1;
            bool new_device_found = false;

            char device_name[50];

            if (peripheral_address != NULL) {
                for(int i = 0; i < devices_count; i++) {
                    if (strcmp((char *)devices[i].address, peripheral_address) == 0) {
                        new_device_found = true;
                        current_device = i;

                        update_device_info(i, gap_cb_param);
                        break;
                    }
                }

                if (new_device_found == false && strcmp(device_to_discover, "") == 0) {
                    current_device = add_device(gap_cb_param);
                }
            }

            // Start device discovering
            if (is_discovering == true && strcmp((char*)devices[current_device].address, device_to_discover) == 0) {
                is_discovering = false;
                ESP_LOGI(DEBUG_PRINT, "Searched device %s\n", peripheral_address);
                discover_device(current_device, gap_cb_param);
            }
            break;

        // Inquiry completed
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            show_found_devices();
            ESP_LOGI(DEBUG_PRINT, "Restarting scanning ------------------------------------------------");
            esp_ble_gap_start_scanning(SCANNING_DURATION);
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

void app_main(void) {

    // ESP_ERROR_CHECK checks outcome of a function,
    // if outcome is other than ESP_OK (0) then error is thrown 

    // Flash initialization
    ESP_ERROR_CHECK(nvs_flash_init());

    // Releasing controller memory
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Initializing Bluetooth controller
    esp_bt_controller_config_t bt_controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_controller_config);

    // Enabling Bluetooth controller
    esp_bt_controller_enable(ESP_BT_MODE_BLE);

    // Initializing, allocating Bluetooth resources
    esp_bluedroid_init();

    // Enabling Bluetooth
    esp_bluedroid_enable();

    // Setting GAP and GATT callbacks
    esp_ble_gap_register_callback(handle_gap_events);
    esp_ble_gattc_register_callback(handle_gatt_events);

    esp_ble_gattc_app_register(0);

    ESP_LOGI(WIFI_DEBUG_PRINT, "ESP_WIFI_MODE_STA");

    // Connect to WiFi
    connect_to_wifi();
}

/***************************************************************************/ /**
 * @file
 * @brief HTTP Client Application
 *******************************************************************************
 * # License
 * <b>Copyright 2022 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "sl_board_configuration.h"
#include "cmsis_os2.h"
#include "sl_wifi.h"
#include "sl_wifi_callback_framework.h"
#include "sl_net.h"
#include "sl_http_client.h"
#include "sl_additional_status.h"
#include "sl_si91x_icm40627.h"
#include "sl_si91x_driver_gpio.h"
#include "rsi_rom_clks.h"
#include <string.h>
#include <stdio.h>

//! Include index html page
#include "index.html.h"
/******************************************************
 *                      Macros
 ******************************************************/
#define CLEAN_HTTP_CLIENT_IF_FAILED(status, client_handle, is_sync, cb_status) \
  do {                                                                         \
    if ((status) != SL_STATUS_OK && (status) != SL_STATUS_IN_PROGRESS) {       \
      sl_http_client_deinit(client_handle);                                    \
      return (((is_sync) == HTTP_SYNC_RESPONSE) ? (status) : (cb_status));     \
    }                                                                          \
  } while (0)

/******************************************************
 *                    Constants
 ******************************************************/
//! HTTP Client Configurations
#define HTTP_CLIENT_USERNAME "admin"
#define HTTP_CLIENT_PASSWORD "admin"

#define IP_VERSION   SL_IPV4
#define HTTP_VERSION SL_HTTP_V_1_1

//! Set 1 - Enable and 0 - Disable
#define HTTPS_ENABLE 0

//! Set 1 - Enable and 0 - Disable
#define EXTENDED_HEADER_ENABLE 0

#if HTTPS_ENABLE
//! Set TLS version
#define TLS_VERSION SL_TLS_V_1_2

#define CERTIFICATE_INDEX SL_HTTPS_CLIENT_CERTIFICATE_INDEX_1

//! Include SSL CA certificate
#include "cacert.pem.h"

//! Load certificate to device flash :
//! Certificate could be loaded once and need not be loaded for every boot up
#define LOAD_CERTIFICATE 1
#endif

//! HTTP request configurations
//! Set HTTP Server IP Address
#define HTTP_SERVER_IP "172.23.7.241" // Change to the IP address of the computer running the server.

//! Set HTTP hostname
#define HTTP_HOSTNAME HTTP_SERVER_IP

//! Server port number
#if HTTPS_ENABLE
//! Set default HTTPS port
#define HTTP_PORT 443
#else
//! Set default HTTP port
#define HTTP_PORT 8080
#endif

//! HTTP resource name
#define HTTP_URL "/coordinates"

//! HTTP post data
#define HTTP_POST_INTERVAL_MS 200U
#define HTTP_JSON_CONTENT_TYPE "application/json"

#if EXTENDED_HEADER_ENABLE
//! Extended headers
#define KEY1 "Content-Type"
#define VAL1 "text/html; charset=utf-8"

#define KEY2 "Content-Encoding"
#define VAL2 "br"

#define KEY3 "Content-Language"
#define VAL3 "de-DE"

#define KEY4 "Content-Location"
#define VAL4 "/index.html"
#endif

//! Application buffer length
#define APP_BUFFER_LENGTH 2000

#define HTTP_SUCCESS_RESPONSE 1
#define HTTP_FAILURE_RESPONSE 2

#define HTTP_STATUS_CLIENT_ERROR_MIN 400U //! HTTP 4xx client error codes
#define HTTP_STATUS_SERVER_ERROR_MAX 599U //! HTTP 5xx server error codes
#define HTTP_STATUS_CODE_NONE        0U

#define HTTP_SYNC_RESPONSE  0
#define HTTP_ASYNC_RESPONSE 1

/******************************************************
 *               Variable Definitions
 ******************************************************/
const osThreadAttr_t thread_attributes = {
  .name       = "app",
  .attr_bits  = 0,
  .cb_mem     = 0,
  .cb_size    = 0,
  .stack_mem  = 0,
  .stack_size = 3072,
  .priority   = osPriorityLow,
  .tz_module  = 0,
};

static const sl_wifi_device_configuration_t http_client_configuration = {
  .boot_option = LOAD_NWP_FW,
  .mac_address = NULL,
  .band        = SL_SI91X_WIFI_BAND_2_4GHZ,
  .boot_config = { .oper_mode                  = SL_SI91X_CLIENT_MODE,
                   .coex_mode                  = SL_SI91X_WLAN_ONLY_MODE,
                   .feature_bit_map            = (SL_WIFI_FEAT_SECURITY_PSK),
                   .tcp_ip_feature_bit_map     = (SL_SI91X_TCP_IP_FEAT_DHCPV4_CLIENT | SL_SI91X_TCP_IP_FEAT_HTTP_CLIENT
                                              | SL_SI91X_TCP_IP_FEAT_DNS_CLIENT | SL_SI91X_TCP_IP_FEAT_SSL),
                   .custom_feature_bit_map     = SL_WIFI_SYSTEM_CUSTOM_FEAT_EXTENSION_VALID,
                   .ext_custom_feature_bit_map = (SL_SI91X_EXT_FEAT_XTAL_CLK | MEMORY_CONFIG
#ifdef SLI_SI917
                                                  | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0
#endif
                                                  ),
                   .bt_feature_bit_map      = 0,
                   .ble_feature_bit_map     = 0,
                   .ble_ext_feature_bit_map = 0,
                   .config_feature_bit_map  = 0 },
  .ta_pool         = { .tx_ratio_in_buffer_pool = 0, .rx_ratio_in_buffer_pool = 0, .global_ratio_in_buffer_pool = 0 },
  .efuse_data_type = SL_SI91X_EFUSE_MFG_SW_VERSION,
  .nwp_fw_image_number = SL_SI91X_NWP_FW_IMAGE_NUMBER_0
};

//! Application buffer
uint8_t app_buffer[APP_BUFFER_LENGTH] = { 0 };

//! Application buffer index
uint32_t app_buff_index = 0;

volatile uint8_t http_rsp_received = 0;
volatile uint8_t end_of_file       = 0;
sl_status_t callback_status        = SL_STATUS_OK;
volatile uint16_t http_response_code = 0;
int32_t offset                     = 0;
int32_t chunk_length               = 0;
static sl_ssi_handle_t accelerometer_ssi_handle;
/******************************************************
 *               Function Declarations
 ******************************************************/
static void application_start(void *argument);
sl_status_t http_client_application(void);
sl_status_t http_response_status(volatile uint8_t *response);
sl_status_t http_put_response_callback_handler(const sl_http_client_t *client,
                                               sl_http_client_event_t event,
                                               void *data,
                                               void *request_context);
sl_status_t http_get_response_callback_handler(const sl_http_client_t *client,
                                               sl_http_client_event_t event,
                                               void *data,
                                               void *request_context);
sl_status_t http_post_response_callback_handler(const sl_http_client_t *client,
                                                sl_http_client_event_t event,
                                                void *data,
                                                void *request_context);
static void reset_http_handles(void);
static sl_status_t wifi_join_event_handler(sl_wifi_event_t event,
                                           sl_status_t status,
                                           char *data,
                                           uint32_t data_length,
                                           void *optional_arg);
static sl_status_t motion_sensor_init(void);

/******************************************************
 *               Function Definitions
 ******************************************************/
static sl_status_t wifi_join_event_handler(sl_wifi_event_t event,
                                           sl_status_t status,
                                           char *data,
                                           uint32_t data_length,
                                           void *optional_arg)
{
  (void)optional_arg;
  const uint8_t join_result = (data != NULL && data_length > 0) ? data[0] : 0;
  SL_DEBUG_LOG_V2(INFO,
                 "Wi-Fi join event: event=0x%lx status=0x%lx result=%c\r\n",
                 (uint32_t)event,
                 (uint32_t)status,
                 join_result != 0 ? join_result : '?');
  return SL_STATUS_OK;
}

static sl_status_t motion_sensor_init(void)
{
  const uint32_t ssi_slave_number = SSI_SLAVE_0;
  sl_status_t status;
  uint8_t device_id = 0;

  if (sl_si91x_gpio_driver_get_uulp_npss_pin(SENSOR_ENABLE_GPIO_PIN) != 1) {
    status = sl_si91x_gpio_driver_enable_clock((sl_si91x_gpio_select_clock_t)ULPCLK_GPIO);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "Sensor GPIO clock enable failed: 0x%lx\r\n", (uint32_t)status);
      return status;
    }
    status = sl_si91x_gpio_driver_set_uulp_npss_pin_mux(SENSOR_ENABLE_GPIO_PIN, NPSS_GPIO_PIN_MUX_MODE0);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "Sensor GPIO mux setup failed: 0x%lx\r\n", (uint32_t)status);
      return status;
    }
    status = sl_si91x_gpio_driver_set_uulp_npss_direction(SENSOR_ENABLE_GPIO_PIN,
                                                           (sl_si91x_gpio_direction_t)GPIO_OUTPUT);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "Sensor GPIO direction setup failed: 0x%lx\r\n", (uint32_t)status);
      return status;
    }
    status = sl_si91x_gpio_driver_set_uulp_npss_pin_value(SENSOR_ENABLE_GPIO_PIN, SET);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "Sensor power enable failed: 0x%lx\r\n", (uint32_t)status);
      return status;
    }
  }

  status = sl_si91x_icm40627_ssi_interface_init(&accelerometer_ssi_handle, ssi_slave_number);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ICM-40627 SSI initialization failed: 0x%lx\r\n", (uint32_t)status);
    return status;
  }
  status = sl_si91x_icm40627_software_reset(accelerometer_ssi_handle);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "ICM-40627 reset failed: 0x%lx\r\n", (uint32_t)status);
    return status;
  }
  status = sl_si91x_icm40627_get_device_id(accelerometer_ssi_handle, &device_id);
  if (status != SL_STATUS_OK || device_id != ICM40627_DEVICE_ID) {
    SL_DEBUG_LOG_V2(ERROR, "ICM-40627 ID check failed: status=0x%lx id=0x%x\r\n",
                    (uint32_t)status, device_id);
    return (status == SL_STATUS_OK) ? SL_STATUS_FAIL : status;
  }
  status = sl_si91x_icm40627_init(accelerometer_ssi_handle);
  if (status == SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(INFO, "ICM-40627 accelerometer initialized\r\n");
  }
  return status;
}

void app_init(void)
{
  osThreadNew((osThreadFunc_t)application_start, NULL, &thread_attributes);
}

static void application_start(void *argument)
{
  UNUSED_PARAMETER(argument);
  sl_status_t status;

  status = sl_net_init(SL_NET_WIFI_CLIENT_INTERFACE, &http_client_configuration, NULL, NULL);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to start Wi-Fi client interface: 0x%lx\r\n", status);
    return;
  }
  SL_DEBUG_LOG_V2(INFO, "Wi-Fi Init Success\r\n");

  status = sl_wifi_set_join_callback_v2(wifi_join_event_handler, NULL);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Failed to register Wi-Fi join callback: 0x%lx\r\n", status);
  }

  sl_mac_address_t station_mac = { 0 };
  status = sl_wifi_get_mac_address(SL_WIFI_CLIENT_2_4GHZ_INTERFACE, &station_mac);
  if (status == SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(INFO, "Station MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                    station_mac.octet[0],
                    station_mac.octet[1],
                    station_mac.octet[2],
                    station_mac.octet[3],
                    station_mac.octet[4],
                    station_mac.octet[5]);
  } else {
    SL_DEBUG_LOG_V2(ERROR, "Failed to read station MAC: 0x%lx\r\n", status);
  }

  status = sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, 0);
  if (status != SL_STATUS_OK) {
    sl_status_t ipv4_status = SL_STATUS_OK;
    sl_status_t ipv6_status = SL_STATUS_OK;
    (void)sl_wifi_get_ip_config_failure_reason(&ipv4_status, &ipv6_status);
    SL_DEBUG_LOG_V2(ERROR, "Failed to bring Wi-Fi client interface up: 0x%lx\r\n", status);
    SL_DEBUG_LOG_V2(ERROR, "IP config status: IPv4=0x%lx IPv6=0x%lx\r\n", ipv4_status, ipv6_status);
    sl_net_ip_address_t ip_address = { 0 };
    const sl_status_t ip_status = sl_net_get_ip_address(SL_NET_WIFI_CLIENT_INTERFACE, &ip_address, 1000);
    if (ip_status == SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(INFO, "Board IPv4: %u.%u.%u.%u\r\n",
                      ip_address.v4.ip_address.bytes[0],
                      ip_address.v4.ip_address.bytes[1],
                      ip_address.v4.ip_address.bytes[2],
                      ip_address.v4.ip_address.bytes[3]);
    } else {
      SL_DEBUG_LOG_V2(INFO, "Board IP query failed: 0x%lx\r\n", ip_status);
    }
    return;
  }
  SL_DEBUG_LOG_V2(INFO, "Wi-Fi Client Connected\r\n");

  status = motion_sensor_init();
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Onboard accelerometer initialization failed: 0x%lx\r\n", (uint32_t)status);
    return;
  }

#if HTTPS_ENABLE && LOAD_CERTIFICATE
  // Load SSL CA certificate
  status = sl_net_set_credential(SL_NET_TLS_SERVER_CREDENTIAL_ID(CERTIFICATE_INDEX),
                                 SL_NET_SIGNING_CERTIFICATE,
                                 cacert,
                                 sizeof(cacert) - 1);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Loading TLS CA certificate in to FLASH Failed, Error Code : 0x%lX\r\n", status);
    return;
  }
  SL_DEBUG_LOG_V2(INFO, "Load TLS CA certificate at index %d Success\r\n", CERTIFICATE_INDEX);
#endif

  status = http_client_application();
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "Unexpected error while HTTP client operation: 0x%lX\r\n", status);
    return;
  }
  SL_DEBUG_LOG_V2(INFO, "Application Demonstration Completed Successfully!\r\n");
}

sl_status_t http_client_application(void)
{
  sl_status_t status;
  sl_http_client_t client_handle = 0;
  sl_http_client_configuration_t client_configuration = { 0 };
  sl_http_client_request_t client_request = { 0 };
  char post_body[96];
  float accel[3];

  const uint16_t username_length = (uint16_t)strlen(HTTP_CLIENT_USERNAME);
  const uint16_t password_length = (uint16_t)strlen(HTTP_CLIENT_PASSWORD);
  const uint32_t credential_size = sizeof(sl_http_client_credentials_t) + username_length + password_length;
  sl_http_client_credentials_t *client_credentials = (sl_http_client_credentials_t *)malloc(credential_size);
  if (client_credentials == NULL) {
    return SL_STATUS_ALLOCATION_FAILED;
  }
  memset(client_credentials, 0, credential_size);
  client_credentials->username_length = username_length;
  client_credentials->password_length = password_length;
  memcpy(client_credentials->data, HTTP_CLIENT_USERNAME, username_length);
  memcpy(client_credentials->data + username_length, HTTP_CLIENT_PASSWORD, password_length);

  status = sl_net_set_credential(SL_NET_HTTP_CLIENT_CREDENTIAL_ID(0),
                                 SL_NET_HTTP_CLIENT_CREDENTIAL,
                                 client_credentials,
                                 credential_size);
  if (status != SL_STATUS_OK) {
    free(client_credentials);
    SL_DEBUG_LOG_V2(ERROR, "HTTP client credential setup failed: 0x%lx\r\n", (uint32_t)status);
    return status;
  }

  client_configuration.network_interface = SL_NET_WIFI_CLIENT_INTERFACE;
  client_configuration.ip_version = IP_VERSION;
  client_configuration.http_version = HTTP_VERSION;
#if HTTPS_ENABLE
  client_configuration.https_enable = true;
  client_configuration.tls_version = TLS_VERSION;
  client_configuration.certificate_index = CERTIFICATE_INDEX;
#endif

  client_request.ip_address = (uint8_t *)HTTP_SERVER_IP;
  client_request.host_name = (uint8_t *)HTTP_HOSTNAME;
  client_request.port = HTTP_PORT;
  client_request.resource = (uint8_t *)HTTP_URL;
  client_request.extended_header = NULL;

  status = sl_http_client_init(&client_configuration, &client_handle);
  if (status != SL_STATUS_OK) {
    SL_DEBUG_LOG_V2(ERROR, "HTTP client init failed: 0x%lx\r\n", (uint32_t)status);
    free(client_credentials);
    return status;
  }
  SL_DEBUG_LOG_V2(INFO, "HTTP client ready for POST to %s:%u%s\r\n", HTTP_SERVER_IP, HTTP_PORT, HTTP_URL);

  status = sl_http_client_add_header(&client_request, "Content-Type", HTTP_JSON_CONTENT_TYPE);
  if (status != SL_STATUS_OK) {
    (void)sl_http_client_deinit(&client_handle);
    free(client_credentials);
    return status;
  }

  for (;;) {
    status = sl_si91x_icm40627_get_accel_data(accelerometer_ssi_handle, accel);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "Accelerometer sample failed: 0x%lx\r\n", (uint32_t)status);
      osDelay(HTTP_POST_INTERVAL_MS);
      continue;
    }

    /* Send milli-g integers to avoid depending on float printf support. */
    const int32_t x_mg = (int32_t)(accel[0] * 1000.0f);
    const int32_t y_mg = (int32_t)(accel[1] * 1000.0f);
    const int32_t z_mg = (int32_t)(accel[2] * 1000.0f);
    (void)snprintf(post_body, sizeof(post_body), "{\"x\":%ld,\"y\":%ld,\"z\":%ld,\"unit\":\"mg\"}",
                   (long)x_mg, (long)y_mg, (long)z_mg);
    SL_DEBUG_LOG_V2(INFO, "Acceleration (mg): x=%ld y=%ld z=%ld\r\n",
                    (long)x_mg, (long)y_mg, (long)z_mg);

    reset_http_handles();
    client_request.http_method_type = SL_HTTP_POST;
    client_request.body = (uint8_t *)post_body;
    client_request.body_length = (uint16_t)strlen(post_body);

    status = sl_http_client_request_init(&client_request,
                                         http_post_response_callback_handler,
                                         "ICM-40627 acceleration");
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "POST request initialization failed: 0x%lx\r\n", (uint32_t)status);
      break;
    }
    status = sl_http_client_send_request(&client_handle, &client_request);
    if (status != SL_STATUS_OK && status != SL_STATUS_IN_PROGRESS) {
      SL_DEBUG_LOG_V2(ERROR, "POST send failed: 0x%lx\r\n", (uint32_t)status);
      break;
    }
    status = http_response_status(&http_rsp_received);
    if (status != SL_STATUS_OK) {
      SL_DEBUG_LOG_V2(ERROR, "POST response failed: status=0x%lx callback=0x%lx\r\n",
                      (uint32_t)status, (uint32_t)callback_status);
      break;
    }
    SL_DEBUG_LOG_V2(INFO, "Coordinate POST accepted by server (HTTP %u)\r\n", http_response_code);
    osDelay(HTTP_POST_INTERVAL_MS);
  }

  (void)sl_http_client_delete_all_headers(&client_request);
  (void)sl_http_client_deinit(&client_handle);
  free(client_credentials);
  return (status == SL_STATUS_OK) ? SL_STATUS_FAIL : status;
}

/* The original example's one-shot PUT/GET/POST flow is kept below for reference. */
#if 0
sl_status_t http_client_application(void)
{
  sl_status_t status                                  = SL_STATUS_OK;
  sl_http_client_t client_handle                      = 0;
  sl_http_client_configuration_t client_configuration = { 0 };
  sl_http_client_request_t client_request             = { 0 };
  int32_t total_put_data_len                          = sizeof(sl_index) - 1;

  offset       = 0;
  chunk_length = 0;

  //! Set HTTP Client credentials
  uint16_t username_length = strlen(HTTP_CLIENT_USERNAME);
  uint16_t password_length = strlen(HTTP_CLIENT_PASSWORD);
  uint32_t credential_size = sizeof(sl_http_client_credentials_t) + username_length + password_length;

  sl_http_client_credentials_t *client_credentials = (sl_http_client_credentials_t *)malloc(credential_size);
  SL_VERIFY_POINTER_OR_RETURN(client_credentials, SL_STATUS_ALLOCATION_FAILED);
  memset(client_credentials, 0, credential_size);
  client_credentials->username_length = username_length;
  client_credentials->password_length = password_length;

  memcpy(&client_credentials->data[0], HTTP_CLIENT_USERNAME, username_length);
  memcpy(&client_credentials->data[username_length], HTTP_CLIENT_PASSWORD, password_length);

  status = sl_net_set_credential(SL_NET_HTTP_CLIENT_CREDENTIAL_ID(0),
                                 SL_NET_HTTP_CLIENT_CREDENTIAL,
                                 client_credentials,
                                 credential_size);
  if (status != SL_STATUS_OK) {
    free(client_credentials);
    return status;
  }

  //! Fill HTTP client_configurations
  client_configuration.network_interface = SL_NET_WIFI_CLIENT_INTERFACE;
  client_configuration.ip_version        = IP_VERSION;
  client_configuration.http_version      = HTTP_VERSION;
#if HTTPS_ENABLE
  client_configuration.https_enable      = true;
  client_configuration.tls_version       = TLS_VERSION;
  client_configuration.certificate_index = CERTIFICATE_INDEX;
#endif

  //! Fill HTTP client_request configurations
  client_request.ip_address      = (uint8_t *)HTTP_SERVER_IP;
  client_request.host_name       = (uint8_t *)HTTP_HOSTNAME;
  client_request.port            = HTTP_PORT;
  client_request.resource        = (uint8_t *)HTTP_URL;
  client_request.extended_header = NULL;

  status = sl_http_client_init(&client_configuration, &client_handle);
  VERIFY_STATUS_AND_RETURN(status);
  SL_DEBUG_LOG_V2(INFO, "HTTP Client init success\r\n");

  sl_http_client_tcp_tls_advanced_options_t tcp_tls_opts = {
    .tcp_keepalive_initial_time_sec   = 120,
    .tcp_max_retry_count              = 5,
    .max_retransmission_timeout_value = 2,
    .ssl_ciphers_bitmap               = 0,
    .ssl_ext_ciphers_bitmap           = 0,
  };
  status = sl_http_client_set_tcp_tls_advanced_configuration(&client_handle, &tcp_tls_opts);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  SL_DEBUG_LOG_V2(INFO, "HTTP Client TCP/TLS advanced configuration set\r\n");

#if EXTENDED_HEADER_ENABLE
  //! Add extended headers
  status = sl_http_client_add_header(&client_request, KEY1, VAL1);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);

  status = sl_http_client_add_header(&client_request, KEY2, VAL2);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);

  status = sl_http_client_add_header(&client_request, KEY3, VAL3);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);

  status = sl_http_client_add_header(&client_request, KEY4, VAL4);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
#endif

  //! Configure HTTP PUT request
  client_request.http_method_type = SL_HTTP_PUT;
  client_request.body             = NULL;
  client_request.body_length      = total_put_data_len;

  //! Initialize callback method for HTTP PUT request
  status = sl_http_client_request_init(&client_request, http_put_response_callback_handler, "This is HTTP client");
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  SL_DEBUG_LOG_V2(INFO, "HTTP PUT request init success\r\n");

  //! Send HTTP PUT request
  status = sl_http_client_send_request(&client_handle, &client_request);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  if (http_rsp_received != HTTP_SUCCESS_RESPONSE) {
    status = http_response_status(&http_rsp_received);
    CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_ASYNC_RESPONSE, callback_status);
  }

  //! Write HTTP PUT data
  while (!end_of_file) {
    //! Get the current length that you want to send
    chunk_length = ((total_put_data_len - offset) > SL_HTTP_CLIENT_MAX_WRITE_BUFFER_LENGTH)
                     ? SL_HTTP_CLIENT_MAX_WRITE_BUFFER_LENGTH
                     : (total_put_data_len - offset);

    if (chunk_length > 0) {
      status = sl_http_client_write_chunked_data(&client_handle, (uint8_t *)(sl_index + offset), chunk_length, 0);

      if (status == SL_STATUS_IN_PROGRESS) {
        status = http_response_status(&http_rsp_received);
        CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_ASYNC_RESPONSE, callback_status);

        offset += chunk_length;
      } else {
        CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
      }
    } else {
      /* All chunks sent; wait for final PUT response (end_of_data) from firmware.
       * That response triggers put_delete() in the callback. Proceeding to GET before
       * this would send GET while NWP still has PUT client active (firmware status 0x15). */
      http_rsp_received = 0;
      status            = http_response_status(&http_rsp_received);
      CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_ASYNC_RESPONSE, callback_status);
    }
  }

  SL_DEBUG_LOG_V2(INFO, "\r\nHTTP PUT request Success!\r\n");
  reset_http_handles();

  //! Configure HTTP GET request
  client_request.http_method_type = SL_HTTP_GET;

  //! Initialize callback method for HTTP GET request
  status = sl_http_client_request_init(&client_request, http_get_response_callback_handler, "This is HTTP client");
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  SL_DEBUG_LOG_V2(INFO, "HTTP Get request init success\r\n");

  //! Send HTTP GET request
  status = sl_http_client_send_request(&client_handle, &client_request);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  if (http_rsp_received != HTTP_SUCCESS_RESPONSE) {
    status = http_response_status(&http_rsp_received);
    CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_ASYNC_RESPONSE, callback_status);
  }

  SL_DEBUG_LOG_V2(INFO, "\r\nHTTP GET request Success\r\n");
  reset_http_handles();

  //! Configure HTTP POST request
  client_request.http_method_type = SL_HTTP_POST;
  client_request.body             = (uint8_t *)HTTP_DATA;
  client_request.body_length      = strlen(HTTP_DATA);

  //! Initialize callback method for HTTP POST request
  status = sl_http_client_request_init(&client_request, http_post_response_callback_handler, "This is HTTP client");
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  SL_DEBUG_LOG_V2(INFO, "HTTP Post request init success\r\n");

  //! Send HTTP POST request
  status = sl_http_client_send_request(&client_handle, &client_request);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
  if (http_rsp_received != HTTP_SUCCESS_RESPONSE) {
    status = http_response_status(&http_rsp_received);
    CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_ASYNC_RESPONSE, callback_status);
  }

  SL_DEBUG_LOG_V2(INFO, "\r\nHTTP POST request Success\r\n");
  reset_http_handles();

#if EXTENDED_HEADER_ENABLE
  status = sl_http_client_delete_all_headers(&client_request);
  CLEAN_HTTP_CLIENT_IF_FAILED(status, &client_handle, HTTP_SYNC_RESPONSE, callback_status);
#endif

  status = sl_http_client_deinit(&client_handle);
  VERIFY_STATUS_AND_RETURN(status);
  SL_DEBUG_LOG_V2(INFO, "HTTP Client deinit success\r\n");
  free(client_credentials);

  return status;
}
#endif

sl_status_t http_put_response_callback_handler(const sl_http_client_t *client,
                                               sl_http_client_event_t event,
                                               void *data,
                                               void *request_context)
{
  UNUSED_PARAMETER(client);
  UNUSED_PARAMETER(event);

  sl_http_client_response_t *put_response = (sl_http_client_response_t *)data;
  callback_status                         = put_response->status;

  SL_DEBUG_LOG_V2(INFO, "\r\n===========HTTP PUT RESPONSE START===========\r\n");
  SL_DEBUG_LOG_V2(INFO, "> Status: 0x%X ", put_response->status);
  SL_DEBUG_LOG_V2(INFO, "> PUT response: %u ", put_response->http_response_code);
  SL_DEBUG_LOG_V2(INFO, "> End of data: %lu ", (unsigned long)put_response->end_of_data);
  SL_DEBUG_LOG_V2(INFO, "> Data Length: %u ", put_response->data_length);
  if (request_context != NULL) {
    SL_DEBUG_LOG_V2(INFO, "> Request Context: %s ", (const char *)request_context);
  }

  if (put_response->status != SL_STATUS_OK && put_response->status != SL_STATUS_IN_PROGRESS) {
    http_rsp_received = HTTP_FAILURE_RESPONSE;
    return put_response->status;
  }

  if (put_response->data_length) {
    if (APP_BUFFER_LENGTH > (app_buff_index + put_response->data_length)) {
      memcpy(app_buffer + app_buff_index, put_response->data_buffer, put_response->data_length);
    }
    app_buff_index += put_response->data_length;
  }
  http_rsp_received = HTTP_SUCCESS_RESPONSE;

  if (put_response->end_of_data == SL_HTTP_CLIENT_PUT_SERVER_RESPONSE_END_OF_DATA) {
    end_of_file = HTTP_SUCCESS_RESPONSE;
  }

  return SL_STATUS_OK;
}

sl_status_t http_get_response_callback_handler(const sl_http_client_t *client,
                                               sl_http_client_event_t event,
                                               void *data,
                                               void *request_context)
{
  UNUSED_PARAMETER(client);
  UNUSED_PARAMETER(event);

  sl_http_client_response_t *get_response = (sl_http_client_response_t *)data;
  callback_status                         = get_response->status;

  SL_DEBUG_LOG_V2(INFO, "\r\n===========HTTP GET RESPONSE START===========\r\n");
  SL_DEBUG_LOG_V2(INFO, "> Status: 0x%X ", get_response->status);
  SL_DEBUG_LOG_V2(INFO, "> GET response: %u ", get_response->http_response_code);
  SL_DEBUG_LOG_V2(INFO, "> End of data: %lu ", (unsigned long)get_response->end_of_data);
  SL_DEBUG_LOG_V2(INFO, "> Data Length: %u ", get_response->data_length);
  if (request_context != NULL) {
    SL_DEBUG_LOG_V2(INFO, "> Request Context: %s ", (const char *)request_context);
  }

  if (get_response->status != SL_STATUS_OK && get_response->status != SL_STATUS_IN_PROGRESS) {
    http_rsp_received = HTTP_FAILURE_RESPONSE;
    callback_status   = SL_STATUS_FAIL;
    return get_response->status;
  }

  if (get_response->http_response_code >= HTTP_STATUS_CLIENT_ERROR_MIN
      && get_response->http_response_code <= HTTP_STATUS_SERVER_ERROR_MAX
      && get_response->http_response_code != HTTP_STATUS_CODE_NONE) {
    http_rsp_received = HTTP_FAILURE_RESPONSE;
    /* Keep actual firmware status for diagnostics (e.g. 0x10015 = invalid state after PUT). */
    callback_status = get_response->status;
    return get_response->status;
  }

  if (get_response->data_length) {
    if (APP_BUFFER_LENGTH > (app_buff_index + get_response->data_length)) {
      memcpy(app_buffer + app_buff_index, get_response->data_buffer, get_response->data_length);
    }
    app_buff_index += get_response->data_length;
  }

  if (get_response->end_of_data) {
    http_rsp_received = HTTP_SUCCESS_RESPONSE;
  }

  SL_DEBUG_LOG_V2(INFO, "GET Data response: %s Offset: %ld\r\n", (const char *)app_buffer, (long)app_buff_index);
  app_buff_index = 0;

  return SL_STATUS_OK;
}

sl_status_t http_post_response_callback_handler(const sl_http_client_t *client,
                                                sl_http_client_event_t event,
                                                void *data,
                                                void *request_context)
{
  UNUSED_PARAMETER(client);
  UNUSED_PARAMETER(event);

  sl_http_client_response_t *post_response = (sl_http_client_response_t *)data;
  callback_status                          = post_response->status;
  http_response_code                       = post_response->http_response_code;

  SL_DEBUG_LOG_V2(INFO, "\r\n===========HTTP POST RESPONSE START===========\r\n");
  SL_DEBUG_LOG_V2(INFO, "> Status: 0x%X ", post_response->status);
  SL_DEBUG_LOG_V2(INFO, "> POST response: %u ", post_response->http_response_code);
  SL_DEBUG_LOG_V2(INFO, "> End of data: %lu ", (unsigned long)post_response->end_of_data);
  SL_DEBUG_LOG_V2(INFO, "> Data Length: %u ", post_response->data_length);
  if (request_context != NULL) {
    SL_DEBUG_LOG_V2(INFO, "> Request Context: %s ", (const char *)request_context);
  }

  if (post_response->status != SL_STATUS_OK && post_response->status != SL_STATUS_IN_PROGRESS) {
    http_rsp_received = HTTP_FAILURE_RESPONSE;
    return post_response->status;
  }

  if (post_response->http_response_code >= HTTP_STATUS_CLIENT_ERROR_MIN
      && post_response->http_response_code <= HTTP_STATUS_SERVER_ERROR_MAX
      && post_response->http_response_code != HTTP_STATUS_CODE_NONE) {
    http_rsp_received = HTTP_FAILURE_RESPONSE;
    return post_response->status;
  }

  if (post_response->end_of_data) {
    http_rsp_received = HTTP_SUCCESS_RESPONSE;
  }

  return SL_STATUS_OK;
}

sl_status_t http_response_status(volatile uint8_t *response)
{
  while (*response != HTTP_SUCCESS_RESPONSE && *response != HTTP_FAILURE_RESPONSE) {
    osDelay(1);
  }

  if (*response != HTTP_SUCCESS_RESPONSE) {
    return SL_STATUS_FAIL;
  }

  // Reset response
  *response = 0;

  return SL_STATUS_OK;
}

static void reset_http_handles(void)
{
  app_buff_index    = 0;
  end_of_file       = 0;
  http_rsp_received = 0;
  callback_status   = SL_STATUS_OK;
  http_response_code = 0;
  offset            = 0;
  chunk_length      = 0;
}

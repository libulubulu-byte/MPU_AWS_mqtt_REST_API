/* webprov.h - SoftAP web provisioning (WiFi only; everything else lives in
 * main/app_config.h and main/aws_certs.h as compile-time macros). */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the httpd and register the config page. net_ap_start() must have been
 * called beforehand so the SoftAP interface is already up. */
esp_err_t webprov_start(void);

#ifdef __cplusplus
}
#endif

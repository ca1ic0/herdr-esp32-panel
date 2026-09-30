#pragma once

/* WiFi station helper: connects and reports the acquired IP. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise NVS/netif/event loop and start connecting in station mode
 *  using the credentials from app_config. No-op while provisioning. */
void wifi_connect_start(void);

/** True once the station has an IP address. */
bool wifi_is_connected(void);

/** Copy the current IP string ("" if not connected). */
void wifi_get_ip(char *buf, int buflen);

#ifdef __cplusplus
}
#endif

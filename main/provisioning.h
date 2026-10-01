#pragma once

/*
 * First-boot provisioning: WPA2 SoftAP + captive portal web page.
 *
 * SoftAP uses a random temporary password (shown on screen and encoded in
 * the Wi-Fi join QR). The QR never contains home Wi-Fi credentials or the
 * gateway token. After save, SoftAP/DNS/HTTP are torn down.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP (WPA2) + DNS + HTTP config server. */
void provisioning_start(void);

/** Stop SoftAP/DNS/HTTP and wipe the temporary password from RAM. */
void provisioning_stop(void);

/** True while provisioning mode is active. */
bool provisioning_active(void);

/** SoftAP SSID (e.g. "HerdrPanel-3FA2"). */
void provisioning_get_ap_ssid(char *buf, int buflen);

/** Temporary SoftAP password (shown on screen only during provisioning). */
void provisioning_get_ap_pass(char *buf, int buflen);

/**
 * Wi-Fi join QR payload (WPA2). Contains only the device AP SSID + temp
 * password — never home Wi-Fi or gateway credentials.
 */
void provisioning_get_qr_payload(char *buf, int buflen);

#ifdef __cplusplus
}
#endif

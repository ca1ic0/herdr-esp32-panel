#pragma once

/*
 * First-boot provisioning: SoftAP + captive portal web page.
 *
 * The device starts an open AP "HerdrPanel-XXXX". The user scans the QR
 * code shown on screen to join it, the phone pops the portal page (via
 * DNS hijack), and the page submits WiFi credentials + backend address.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP + DNS + HTTP config server. */
void provisioning_start(void);

/** True while provisioning mode is active. */
bool provisioning_active(void);

/** Get the SoftAP SSID (e.g. "HerdrPanel-3FA2"), used for the QR code. */
void provisioning_get_ap_ssid(char *buf, int buflen);

/**
 * Build the QR payload. Encodes a WiFi join string so the phone connects
 * to the AP with one scan; the captive portal opens automatically after.
 */
void provisioning_get_qr_payload(char *buf, int buflen);

#ifdef __cplusplus
}
#endif

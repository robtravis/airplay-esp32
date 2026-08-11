#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

/**
 * Initialize WiFi in both AP and STA modes
 * @param ap_ssid AP SSID (if NULL, uses default)
 * @param ap_password AP password (if NULL, uses default or open)
 */
void wifi_init_apsta(const char *ap_ssid, const char *ap_password);

/**
 * Block until WiFi is connected and has an IP address
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return true if connected, false if timeout
 */
bool wifi_wait_connected(uint32_t timeout_ms);

/**
 * Get the device MAC address as a string (XX:XX:XX:XX:XX:XX)
 */
void wifi_get_mac_str(char *mac_str, size_t len);

/**
 * Check if WiFi STA is connected
 */
bool wifi_is_connected(void);

/**
 * Get current IP address as string
 * @param ip_str Output buffer
 * @param len Buffer size
 * @return ESP_OK on success
 */
esp_err_t wifi_get_ip_str(char *ip_str, size_t len);

/**
 * Scan for available WiFi networks
 * @param ap_list Output array of AP info (caller must free)
 * @param ap_count Output: number of APs found
 * @return ESP_OK on success
 */
esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count);

/**
 * Disconnect and stop WiFi
 */
void wifi_stop(void);

/**
 * Erase the stored network and bring the setup AP back up, in place.
 *
 * Does not reboot: a warm reset leaves these boards dark until physically
 * unplugged, which would make the recovery path look like a failure. The device
 * stays running and shows the setup screen.
 */
/**
 * Join a network now, without restarting.
 *
 * Provisioning used to save the credentials and call esp_restart(), which on this
 * board is a warm reset that does not boot — so setup appeared to hang and the
 * device had to be physically unplugged to come back. The AP stays up until the
 * STA gets an address, and the got-IP handler takes the setup screen down.
 */
esp_err_t wifi_connect_to(const char *ssid, const char *password);

void wifi_forget_network(void);

/**
 * Same, on a dedicated task. Use this from UI callers: the work needs more stack
 * than the encoder or HTTP task has, and it repaints the screen as it goes.
 */
void wifi_forget_network_async(void);

/**
 * The sanitized hostname, i.e. the "<name>.local" the device answers to. Useful
 * on screen: it saves the user having to find an IP address.
 */
void wifi_get_hostname(char *out, size_t len);

/**
 * Set the DHCP hostname from the given device name.
 * Sanitizes to a valid DNS label (lowercase, hyphens for spaces/symbols).
 * Takes effect on the next DHCP transaction.
 */
void wifi_set_hostname(const char *device_name);

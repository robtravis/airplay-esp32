#pragma once

#include "sdkconfig.h"
#include <stdbool.h>

/**
 * OLED display module - Shows track metadata, playback position &
 * progress bar. Registers as an RTSP event observer to receive metadata
 * updates automatically.
 *
 * When CONFIG_DISPLAY_ENABLED is not set, display_init() is an inline no-op
 * and no display code is compiled or linked.
 */

#ifdef CONFIG_DISPLAY_ENABLED

/**
 * Initialize the OLED display and register for RTSP events.
 *
 * @param bus  Pre-initialised bus handle to share with the board:
 *             - I2C mode: pass an i2c_master_bus_handle_t
 *             - SPI mode: pass (void*)(intptr_t)spi_host_device_t
 *             Pass NULL to let the display component initialise its own bus
 *             (uses the GPIO pins from Kconfig).
 */
void display_init(void *bus);

#ifdef CONFIG_DISPLAY_DRIVER_ST7789
/**
 * Show WiFi setup instructions: the AP name to join and the address to open.
 *
 * Without this the standby screen reads "AirPlay Ready" on a device that has no
 * network and is waiting to be provisioned, which tells the user nothing about
 * what to do. Declared only for the ST7789 renderer; the OLED path is
 * unaffected.
 */
void display_show_setup(const char *ssid, const char *ip);

/// Leave the setup screen (STA got an address).
void display_clear_setup(void);

/**
 * Draw a menu list. The caller owns navigation and passes the full item list
 * plus the selected index; the display windows it to the rows that fit, the way
 * display_archive_list() did in the radio firmware.
 */
void display_menu_show(const char *header, const char **items, int count,
                       int sel);

/// Leave the menu and return to whatever was on screen before.
void display_menu_hide(void);

/**
 * Show a spectrum visualiser in place of the progress bar.
 *
 * Off by default. It adds a 20fps partial repaint, and display work starving the
 * audio pump is this firmware's known failure mode — so it stays something the
 * user opts into, and the `gaps=`/`outliers=` counters are worth watching after
 * turning it on.
 */
void display_set_visualizer(bool enabled);
bool display_get_visualizer(void);
#else
static inline void display_show_setup(const char *ssid, const char *ip) {
  (void)ssid;
  (void)ip;
}
static inline void display_clear_setup(void) {}
static inline void display_menu_show(const char *header, const char **items,
                                     int count, int sel) {
  (void)header;
  (void)items;
  (void)count;
  (void)sel;
}
static inline void display_menu_hide(void) {}
static inline void display_set_visualizer(bool enabled) {
  (void)enabled;
}
static inline bool display_get_visualizer(void) {
  return false;
}
#endif

#else

static inline void display_init(void *bus) {
  (void)bus;
}
static inline void display_show_setup(const char *ssid, const char *ip) {
  (void)ssid;
  (void)ip;
}
static inline void display_clear_setup(void) {}
static inline void display_menu_show(const char *header, const char **items,
                                     int count, int sel) {
  (void)header;
  (void)items;
  (void)count;
  (void)sel;
}
static inline void display_menu_hide(void) {}
static inline void display_set_visualizer(bool enabled) {
  (void)enabled;
}
static inline bool display_get_visualizer(void) {
  return false;
}

#endif

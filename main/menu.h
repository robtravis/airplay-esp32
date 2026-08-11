/**
 * @file menu.h
 * @brief On-device settings menu, driven by the rotary encoder.
 *
 * Ports the interaction model from the radio firmware's nav.h and the screen
 * state machine in its .ino:
 *
 *   rotate      -> move the selection (volume when no menu is open)
 *   short press -> select        (mute when no menu is open)
 *   long press  -> open / back out
 *
 * The menu owns navigation and actions; the display owns drawing. That split is
 * what lets the list live in LVGL while the model stays testable plain C, and it
 * mirrors how display_archive_list() was fed a flat array of item strings.
 */
#pragma once

#include <stdbool.h>

/// Build the model. Does not draw anything until the menu is opened.
void menu_init(void);

/// True while the menu is on screen — the encoder routes to it rather than to
/// volume, and the button selects rather than mutes.
bool menu_is_open(void);

/// Long press: open the menu, or step back out of a submenu / close it.
void menu_back_or_open(void);

/// Short press: activate the highlighted row.
void menu_select(void);

/// Rotation, in detents. Positive is clockwise.
void menu_scroll(int detents);

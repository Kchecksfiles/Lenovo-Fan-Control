/* Auto mode: switch the fan speed automatically while a chosen app is running.
 *
 * The module is self-contained. The main program only needs to:
 *   - call autoapp_init() once the window exists,
 *   - add a tray menu item (text from autoapp_menu_text()) that calls
 *     autoapp_show_dialog(),
 *   - call autoapp_manual_override() whenever the user changes the speed by hand.
 */
#ifndef AUTOAPP_H
#define AUTOAPP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUTOAPP_SPEED_HIGH = 0,
    AUTOAPP_SPEED_LOW = 1,
    AUTOAPP_SPEED_NORMAL = 2
};

/* Callback used by this module to change the fan speed (runs on the UI thread). */
typedef void (*autoapp_apply_fn)(int speed);

/* True while this module itself is changing the fan speed. */
extern int autoapp_applying;

/* Load saved settings and start monitoring if Auto mode was left enabled. */
void autoapp_init(HWND hwnd, autoapp_apply_fn apply);

/* Show the "select app / fan speed" box. Returns when the box is closed. */
void autoapp_show_dialog(HWND owner);

/* Tell Auto mode the user changed the speed manually (so it won't undo it). */
void autoapp_manual_override(void);

/* Non-zero if Auto mode is currently enabled. */
int autoapp_is_enabled(void);

/* Localized tray menu text. */
const wchar_t* autoapp_menu_text(void);

#ifdef __cplusplus
}
#endif

#endif

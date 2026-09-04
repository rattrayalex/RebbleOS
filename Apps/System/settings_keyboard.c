/* settings_keyboard.c
 * GUI implementation for external (Bluetooth HID) keyboard settings
 * RebbleOS
 *
 * Settings -> Keyboard.  Shows the link state of the external keyboard,
 * starts pairing, reconnects to or forgets the bonded keyboard, and shows
 * the last key pressed as a proof of life for testers.
 */

#include "rebbleos.h"
#include "menu.h"
#include "status_bar_layer.h"
#include "platform_res.h"
#include "keyboard.h"
#include "keyboard_service.h"

static Window *_kbd_window;
static Menu *_kbd_menu;
static StatusBarLayer *_kbd_status_bar;

/* Row subtitles.  MenuItem keeps pointers to these, so they outlive every
 * rebuild of the rows and are rewritten in place. */
static char _status_str[KEYBOARD_NAME_MAX + 16];
static char _last_key_str[16];

static void _refresh(void);

/* Status row subtitle for a link state. */
static void _set_status(KeyboardLinkState state)
{
    const char *name = keyboard_get_name();

    switch (state) {
    case KeyboardLinkScanning:
        strcpy(_status_str, "Scanning...");
        break;
    case KeyboardLinkConnecting:
        strcpy(_status_str, "Connecting...");
        break;
    case KeyboardLinkConnected:
        if (name && name[0])
            snprintf(_status_str, sizeof(_status_str), "Connected: %s", name);
        else
            strcpy(_status_str, "Connected");
        break;
    case KeyboardLinkDisconnected:
    default:
        strcpy(_status_str, "Not connected");
        break;
    }
}

/* "Last key" row subtitle for a key-down event.  A space is printable but
 * would show as an empty row, so it falls through to the usage ID like the
 * keys that have no character. */
static void _set_last_key(const KeyboardEvent *event)
{
    if (event->character > ' ' && event->character < 0x7f)
        snprintf(_last_key_str, sizeof(_last_key_str), "%c", event->character);
    else
        snprintf(_last_key_str, sizeof(_last_key_str), "usage 0x%02x", event->usage);
}

static struct MenuItems *_pair(const struct MenuItem *item)
{
    int rc = hw_keyboard_pair_start();

    APP_LOG("settings", APP_LOG_LEVEL_INFO, "keyboard pair start: %d", rc);
    if (rc != 0)
        strcpy(_status_str, "Pairing failed to start");
    else
        _set_status(keyboard_get_link_state());
    _refresh();
    return NULL;
}

static struct MenuItems *_connect(const struct MenuItem *item)
{
    int rc = hw_keyboard_connect();

    APP_LOG("settings", APP_LOG_LEVEL_INFO, "keyboard connect: %d", rc);
    if (rc != 0)
        strcpy(_status_str, "Connect failed");
    else
        _set_status(keyboard_get_link_state());
    _refresh();
    return NULL;
}

static struct MenuItems *_forget(const struct MenuItem *item)
{
    int rc = hw_keyboard_forget();

    APP_LOG("settings", APP_LOG_LEVEL_INFO, "keyboard forget: %d", rc);
    if (rc != 0)
        strcpy(_status_str, "Forget failed");
    else
        _set_status(keyboard_get_link_state());
    _refresh();
    return NULL;
}

/* Rebuild the rows from the current keyboard state.  menu_set_items() frees
 * the previous rows and resets the selection, so the selected row is saved
 * and restored around it, with the alignment that Up/Down navigation uses
 * so the row stays in view. */
static void _refresh(void)
{
    MenuItems *items;
    MenuIndex sel;

    if (!_kbd_menu)
        return;

    sel = menu_layer_get_selected_index(_kbd_menu->layer);

    if (!hw_keyboard_is_supported()) {
        items = menu_items_create(1);
        menu_items_add(items, MenuItem("Not supported", "on this watch", RESOURCE_ID_SPANNER, NULL));
    } else {
        bool bonded = hw_keyboard_has_bond();

        items = menu_items_create(5);
        menu_items_add(items, MenuItem("Status", _status_str, RESOURCE_ID_SPANNER, NULL));
        menu_items_add(items, MenuItem("Pair new keyboard", "Scan and bond", RESOURCE_ID_SPANNER, _pair));
        if (bonded) {
            menu_items_add(items, MenuItem("Connect", "Reconnect", RESOURCE_ID_SPANNER, _connect));
            menu_items_add(items, MenuItem("Forget keyboard", "Drop the bond", RESOURCE_ID_SPANNER, _forget));
        }
        menu_items_add(items, MenuItem("Last key", _last_key_str, RESOURCE_ID_SPANNER, NULL));
    }

    menu_set_items(_kbd_menu, items);

    if (sel.row >= items->count)
        sel.row = items->count - 1;
    menu_layer_set_selected_index(_kbd_menu->layer, sel, MenuRowAlignCenter, false);
    layer_mark_dirty(menu_get_layer(_kbd_menu));
}

/* Runs on the app thread.  The window stays loaded after Back (a window is
 * unloaded only when it is destroyed), so the rows are rebuilt only while
 * the keyboard window is on the stack; the strings are kept current so the
 * next open shows the right state. */
static void _keyboard_event(KeyboardEvent *event, void *context)
{
    switch (event->type) {
    case KeyboardEventKeyDown:
        _set_last_key(event);
        break;
    case KeyboardEventLinkState:
        _set_status(event->link_state);
        break;
    default:
        return;
    }

    if (window_stack_contains_window(_kbd_window))
        _refresh();
}

static void _kbd_menu_exit(struct Menu *menu, void *context)
{
    window_stack_pop(false);
}

static void _kbd_window_load(Window *window)
{
    Layer *window_layer = window_get_root_layer(window);

#ifdef PBL_RECT
    _kbd_menu = menu_create(GRect(0, 16, DISPLAY_COLS, DISPLAY_ROWS - 16));
#else
    /* Let the menu draw behind the status bar so it is perfectly centered */
    _kbd_menu = menu_create(GRect(0, 0, DISPLAY_COLS, DISPLAY_ROWS));
#endif
    menu_set_callbacks(_kbd_menu, _kbd_menu, (MenuCallbacks) {
        .on_menu_exit = _kbd_menu_exit
    });
    layer_add_child(window_layer, menu_get_layer(_kbd_menu));
    menu_set_click_config_onto_window(_kbd_menu, window);

    _kbd_status_bar = status_bar_layer_create();
    layer_add_child(window_layer, status_bar_layer_get_layer(_kbd_status_bar));

    strcpy(_last_key_str, "None yet");
    _set_status(keyboard_get_link_state());
    _refresh();

    keyboard_service_subscribe(_keyboard_event, NULL);
}

static void _kbd_window_unload(Window *window)
{
    keyboard_service_unsubscribe();
    menu_destroy(_kbd_menu);
    _kbd_menu = NULL;
    status_bar_layer_destroy(_kbd_status_bar);
}

void settings_keyboard_invoke(void)
{
    window_stack_push(_kbd_window, false);

    /* The load handler runs only on the first push; later opens refresh here. */
    _set_status(keyboard_get_link_state());
    _refresh();
}

void settings_keyboard_init(void)
{
    _kbd_window = window_create();
    window_set_window_handlers(_kbd_window, (WindowHandlers) {
        .load = _kbd_window_load,
        .unload = _kbd_window_unload,
    });
}

void settings_keyboard_deinit(void)
{
    window_destroy(_kbd_window);
    _kbd_window = NULL;
}

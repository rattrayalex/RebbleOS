/* keyboard_service.h
 * Event service for external keyboard key and link events
 * RebbleOS
 */
#pragma once

#include "keyboard.h"

typedef enum KeyboardEventType {
    KeyboardEventKeyDown = 0,
    KeyboardEventKeyUp,
    KeyboardEventLinkState,
} KeyboardEventType;

typedef struct KeyboardEvent {
    KeyboardEventType type;
    uint8_t usage;                /* HID usage ID, for key events */
    uint8_t modifiers;            /* modifier bits at the time of the event */
    char character;               /* printable ASCII for the key under the modifiers, or 0 */
    KeyboardLinkState link_state; /* current link state, for every event type */
} KeyboardEvent;

typedef void (*KeyboardHandler)(KeyboardEvent *event, void *context);

/* Subscribe the calling thread (app or overlay) to keyboard events. */
void keyboard_service_subscribe(KeyboardHandler handler, void *context);
void keyboard_service_unsubscribe(void);
void keyboard_service_unsubscribe_thread(app_running_thread *thread);

/* Posted by rcore/keyboard.c.  Thread context only. */
void keyboard_service_post(const KeyboardEvent *event);

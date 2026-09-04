/* keyboard_service.c
 * Event service for external keyboard key and link events
 * RebbleOS
 */

#include "librebble.h"
#include "node_list.h"
#include "event_service.h"
#include "keyboard_service.h"

/* STUB: the real implementation is being written. */

void keyboard_service_subscribe(KeyboardHandler handler, void *context) {
    (void)handler;
    (void)context;
}

void keyboard_service_unsubscribe(void) {
}

void keyboard_service_unsubscribe_thread(app_running_thread *thread) {
    (void)thread;
}

void keyboard_service_post(const KeyboardEvent *event) {
    (void)event;
}

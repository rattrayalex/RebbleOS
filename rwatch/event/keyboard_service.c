/* keyboard_service.c
 * Event service for external keyboard key and link events
 * RebbleOS
 *
 * Modelled on connection_service.c: the subscription is a small record on
 * the subscriber's heap, registered as the event_service context.  Events
 * are posted by rcore/keyboard.c from the service worker thread as heap
 * copies; event_service's destroy callback frees a copy on the last thread
 * that sees it.
 *
 * One subscriber at a time.  event_service_event_trigger() relays and
 * destroys an event once per matching subscriber, so a second subscriber on
 * another thread would free the copy twice; subscribe refuses instead.
 */

#include "librebble.h"
#include "node_list.h"
#include "event_service.h"
#include "keyboard_service.h"
#include "log.h"

#define MODULE_NAME "kbdsvc"
#define MODULE_TYPE "KERN"
#define LOG_LEVEL RBL_LOG_LEVEL_ERROR

typedef struct KeyboardSubscription {
    KeyboardHandler handler;
    void *context;
} KeyboardSubscription;

static KeyboardSubscription *_subscription;   /* NULL while nobody listens */
static app_running_thread *_subscriber_thread; /* owner of _subscription */

static void _keyboard_service_cb(EventServiceCommand command, void *data, void *context)
{
    KeyboardSubscription *sub = (KeyboardSubscription *)context;

    /* A registration the owner left behind: never touch its record. */
    if (sub != _subscription)
        return;
    if (sub->handler)
        sub->handler((KeyboardEvent *)data, sub->context);
}

void keyboard_service_subscribe(KeyboardHandler handler, void *context)
{
    app_running_thread *thread = appmanager_get_current_thread();

    if (!thread)
        return;
    if (_subscription && _subscriber_thread != thread) {
        LOG_ERROR("keyboard already subscribed by %s", _subscriber_thread->thread_name);
        return;
    }

    if (!_subscription) {
        KeyboardSubscription *sub = app_calloc(1, sizeof(KeyboardSubscription));
        if (!sub)
            return;
        _subscription = sub;
        _subscriber_thread = thread;
        event_service_subscribe_with_context(EventServiceCommandKeyboard, _keyboard_service_cb, sub);
    }

    /* Subscribing again from the same thread replaces the handler. */
    _subscription->handler = handler;
    if (_subscription->handler)
        MK_THUMB_CB(_subscription->handler);
    _subscription->context = context;
}

void keyboard_service_unsubscribe(void)
{
    if (!_subscription || _subscriber_thread != appmanager_get_current_thread())
        return;

    event_service_unsubscribe(EventServiceCommandKeyboard);
    app_free(_subscription);
    _subscription = NULL;
    _subscriber_thread = NULL;
}

void keyboard_service_unsubscribe_thread(app_running_thread *thread)
{
    if (!_subscription || _subscriber_thread != thread)
        return;

    event_service_unsubscribe_thread(EventServiceCommandKeyboard, thread);
    remote_free(_subscription); /* lives on the dying thread's heap */
    _subscription = NULL;
    _subscriber_thread = NULL;
}

void keyboard_service_post(const KeyboardEvent *event)
{
    /* With no subscriber, event_service would never destroy the copy. */
    if (!_subscription)
        return;

    KeyboardEvent *copy = malloc(sizeof(KeyboardEvent));
    if (!copy) {
        LOG_ERROR("no memory for keyboard event");
        return;
    }
    *copy = *event;

    /* remote_free also covers event_service_post's queue-full path, which
     * calls the destroy callback on the posting thread. */
    event_service_post(EventServiceCommandKeyboard, copy, remote_free);
}

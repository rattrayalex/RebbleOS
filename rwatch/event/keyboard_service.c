/* keyboard_service.c
 * Event service for external keyboard key and link events
 * RebbleOS
 *
 * Modelled on connection_service.c: the subscription is a small record on
 * the subscriber's heap, registered as the event_service context.  Events
 * are posted by rcore/keyboard.c from the service worker thread, packed
 * into the event_service data word (see _pack), so nothing is allocated
 * per key press.
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

/* An event travels through event_service packed into the data word, so no
 * heap copy is needed: a copy would leak whenever the app or overlay queue
 * drops the message (overlay_window_post_event() posts with no timeout to
 * a queue of depth 1 and ignores the result) or the subscriber goes away
 * between post and delivery. */
#define _PACK_TYPE_SHIFT      0   /* 2 bits */
#define _PACK_USAGE_SHIFT     2   /* 8 bits */
#define _PACK_MODIFIERS_SHIFT 10  /* 8 bits */
#define _PACK_CHAR_SHIFT      18  /* 8 bits */
#define _PACK_LINK_SHIFT      26  /* 2 bits */

static void *_pack(const KeyboardEvent *event)
{
    uintptr_t w = ((uintptr_t)event->type & 3) << _PACK_TYPE_SHIFT
                | ((uintptr_t)event->usage & 0xff) << _PACK_USAGE_SHIFT
                | ((uintptr_t)event->modifiers & 0xff) << _PACK_MODIFIERS_SHIFT
                | ((uintptr_t)(uint8_t)event->character & 0xff) << _PACK_CHAR_SHIFT
                | ((uintptr_t)event->link_state & 3) << _PACK_LINK_SHIFT;
    return (void *)w;
}

static void _unpack(void *data, KeyboardEvent *event)
{
    uintptr_t w = (uintptr_t)data;
    event->type = (KeyboardEventType)((w >> _PACK_TYPE_SHIFT) & 3);
    event->usage = (w >> _PACK_USAGE_SHIFT) & 0xff;
    event->modifiers = (w >> _PACK_MODIFIERS_SHIFT) & 0xff;
    event->character = (char)((w >> _PACK_CHAR_SHIFT) & 0xff);
    event->link_state = (KeyboardLinkState)((w >> _PACK_LINK_SHIFT) & 3);
}

/* event_service_post() calls the destroy callback unconditionally when a
 * queue is full, so it must exist even though there is nothing to free. */
static void _destroy_nothing(void *data)
{
    (void)data;
}

static void _keyboard_service_cb(EventServiceCommand command, void *data, void *context)
{
    KeyboardSubscription *sub = (KeyboardSubscription *)context;
    KeyboardEvent event;

    /* A registration the owner left behind: never touch its record. */
    if (sub != _subscription)
        return;
    if (!sub->handler)
        return;
    _unpack(data, &event);
    sub->handler(&event, sub->context);
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
    if (!_subscription)
        return;
    event_service_post(EventServiceCommandKeyboard, _pack(event), _destroy_nothing);
}

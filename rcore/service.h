/* service.h
 * definitions for service worker thread
 * RebbleOS
 */

#pragma once

typedef void (*service_callback_t)(void *ctx);

void service_init();
/* Queue cbk(ctx) to run on the service thread `when` ticks from now.  May be
 * called from an ISR.  Returns 1 when queued, 0 when the service queue was
 * full and the packet was dropped. */
int service_submit(service_callback_t cbk, void *ctx, uint32_t when);

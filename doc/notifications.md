# Notifications and events

## Overview

Notifications, also called events, aims at facilitating the implementation of the control path of a
given algorithm (type: `dpu_offload_event_t`). In other words, it is only a capability to send 
notifications between execution contexts and while the associated interfaces offer similarities with 
an active message communication layer, it is not meant to be one mainly because missing advanced 
memory management capabilities. 
Active messages are available by using UCX and the endpoints available through the endpoint cache
and the execution contexts.

Notifications are composed of two entities: a header and a payload. The header is based on unique
identifier to uniquely identify the source, the destination and the type of the notification. The
payload can be any buffer; developers can request a buffer allocated by the infrastructure library
or manage their own memory and pass it in the notification system for performance.

The notification type is also used to register before hand a notification handler. Upon reception the
registered handler for the type is invoked, both the header and payload being provided within the
handler. At the moment, only one handler per notification type can be registered but developers are
free to implement handler that will invoke sub-handlers.

It is possible to emit events from within a notification handler.

It is also possible to create a hierarchy of events, i.e., to create sub-event within a given event.
In such a situation, the event containing sub-events must be a local event, i.e., should not be used
to send a notification to a remote execution context and is identified as a *meta-event*. A 
meta-event completes only when all the sub-events are completed.

The notification system (type: `dpu_offload_ev_sys_t`) is the core object used to implement the 
notification system. It provides a pool of free event that are available for use (using the `event_get
()` function). Note that it is possible to request specific features when getting an event, such as
requesting from the library an allocated buffer, by using a *dpu_offload_event_info_t* object.
Please refer to the doxygen documentation for details.

Once an event object is obtained, it is possible to set its payload and emit it. Note that two 
functions are available to emit an event:

- `event_channel_emit()`: which emits an event for which the payload is already specified.
- `event_channel_emit_with_payload()`: which emits an event and specify the payload at emission time.

When the event is emitting, it is by default added to a list of ongoing events. It is possible to
manually manage all events, which will prevent the event to be added to the ongoing list. Please
refer to the doxygen documentation for details.
When the event is on the ongoing list and completes, the event is implicitly returned to the event
system. If the event is manually handled, developers must return it by using the `event_return()`
function.

Please see the doxygen documentation for details about the datastructures and functions related to
the notification system.

## Management of notification handlers

Handlers are a core concept of the notification system: until a handler is correctly registered,
no notification can be delivered. Notifications and handlers are based on a type, meaning upon
reception, a notification is matched with a handler based on the type.

[Examples](#handler-registration) are available

### Registration

Two methods are available to register a notification handler:
1. Register a default handler at the engine level. Once registered, the handler is applied to all execution contexts newly created in the context of the engine.
2. Register a handler at the execution context level.

The signature of the function to register a default handler is:
```
dpu_offload_status_t engine_register_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_cb cb, notification_info_t *info);
```

The signature of the function to register a handler to an execution context:
```
dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb, notification_info_t *info);
```

Both registration functions support an info objects. The data structure of the info object is:
```
typedef void *(*get_buf_fn)(void *pool, void *args);
typedef void (*return_buf_fn)(void *pool, void *buf);

typedef struct notification_info
{
    // Optional function to get a buffer from a pool
    get_buf_fn get_buf;
    // Optional function to return a buffer to a pool
    return_buf_fn return_buf;
    // Memory pool to get notification payload buffer
    void *mem_pool;
    // Optional arguments to pass to the get function
    void *get_buf_args;
    // Size of the elements in the list
    size_t element_size;
} notification_info_t;
```
Note that is includes everything necessary to be able to register a memory pool and associated functions so callers to plug-in
memory pools of objects to ensure high-performance communications. More details about how to use memory pools for notifications
are available [here](#use-of-pool-of-objects-for-high-performance-notifications).

Please refer to the doxygen documentation for all the details about the two registration functions.

### Update a registration

Once a handler is register, it is possible to update the data associated to the registration. As for the registration,
two different functions are available, one to update the default handler at the engine level and one to update the registration
in the context of the execution context.
```
dpu_offload_status_t engine_update_default_notification_handler(offloading_engine_t *engine, uint64_t type, notification_info_t *info);
```

```
dpu_offload_status_t event_channel_update(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_info_t *info);
```

As for the registration functions, the functions to update an existing registration are based on info objects.
This is mainly because most update operations are used to register a new pool of memory for performance
optimization purposes. More details about the management of memory pools are available [here](#use-of-pool-of-objects-for-high-performance-notifications).

## Self-notifications

The notification system supports sending notification to the current execution context, a.k.a.,
self-notifications or sending notifications to self.

The same assumptions are made:
- the handler needs to be registered for the notification to be delivered,
- once the event is returned, the buffer is assumed to not be available any more.

Technically, the infrastructure implements an optimization for self notifications: while
a communication is initiated for standard notifications, for self notifications, the handler
(if available) is invoked right away, by-passing the networking layer within the infrastructure.

Note that if for whatever reason users need to initiate a communication for communication to self,
the execution context also provides an endpoint for the current execution context:
```
offloading_engine_t *engine;
offload_engine_init(&engine);
...
ucp_endpoint_t self_ep = engine->self_ep
```
In a standard configuration, the networking backend is UCX and the endpoint can be directly used
by users with the UCX APIs to initiate communications.

## Local events and sub-events

## Manual return of events

## Use of pool of objects for high-performance notifications

## Examples

### Handler registration

```
#define MY_TEST_NOTIF_ID (1000)

static int notifs_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    fprintf(stdout, "notification of type #%ld received\n", hdr->type);
    return 0;
}

int main(int argc, char **argv)
{
    offloading_engine_t *engine = NULL;
    execution_context_t *econtext;
    offload_engine_init(&engine);
    econtext = client_init(engine, NULL);
    event_channel_register(econtext->event_channels,
                           MY_TEST_NOTIF_ID,
                           notifs_cb,
                           NULL);
    ...
    client_fini(&econtext);
    offload_engine_fini(&engine);
}
```

To register a default notification handler that will be applied to all execution contexts that will then be created:

```
#define MY_TEST_NOTIF_ID (1000)

static int self_notifs_cb(struct dpu_offload_ev_sys *ev_sys, execution_context_t *econtext, am_header_t *hdr, size_t hdr_len, void *data, size_t data_len)
{
    fprintf(stdout, "notification of type #%ld received\n", hdr->type);
    return 0;
}

int main(int argc, char **argv)
{
    offloading_engine_t *engine = NULL;
    offload_engine_init(&engine);
    engine_register_default_notification_handler(engine, MY_TEST_NOTIFS_ID, self_notifs_cb, NULL);
    ...
    offload_engine_fini(&engine);
}
```
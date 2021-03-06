/**
 * Copyright (C) 2018, ControlThings Oy Ab
 * Copyright (C) 2018, André Kaustell
 * Copyright (C) 2018, Jan Nyman
 * Copyright (C) 2018, Jepser Lökfors
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * @license Apache-2.0
 */
#ifndef WISH_EVENT_H
#define WISH_EVENT_H

#include "wish_core.h"
#include "wish_connection.h"

enum wish_event_type {
    WISH_EVENT_CONTINUE, /* Signal to the task indicating a continued message processing */
    WISH_EVENT_NEW_DATA, /* Signal to the task from the tcp recv callback, indicating new data available */
    WISH_EVENT_NEW_CORE_CONNECTION,  /* Signal that a connection to a
    remote core has been established. Normally this would result in
    sending a 'peers' request */
    WISH_EVENT_REQUEST_CONNECTION_CLOSING,
    WISH_EVENT_REQUEST_CONNECTION_ABORT,
};

struct wish_event {
    enum wish_event_type event_type;
    wish_connection_t *context;
    void* metadata;
};

/* Initialize the message processor task */
void wish_message_processor_init(wish_core_t* core);

/* Function implementing the message processor. The parameter ev points
 * the the event which should be processed. */
void wish_message_processor_task(wish_core_t* core, struct wish_event *ev);

struct wish_event * wish_get_next_event(void);

/* Function which can be used to notify the message processor task of an
 * event that has happened */
void wish_message_processor_notify(struct wish_event *ev);

/* This function is called when a new service is first detected */
void wish_report_new_service(wish_connection_t *ctx, uint8_t *wsid, 
    char *protocol_name_str);

/* This function is called when a Wish service goes up or down */
void wish_report_service_status_change(wish_connection_t *ctx, uint8_t *wsid, 
    bool online);

#endif

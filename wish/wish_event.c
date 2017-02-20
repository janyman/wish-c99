#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "wish_core.h"
#include "wish_event.h"
#include "wish_debug.h"
#include "wish_rpc.h"
#include "wish_connection_mgr.h"
#include "wish_core_rpc_func.h"
#include "wish_identity.h"
#include "wish_utils.h"



/* This task will be set up at the by of message_processor_task_init().
 * The OS task system will be notified from tcp_recv_cb, 
 * when new data becomes available. The Task system will then call this
 * function. Note that this not a thread, the system is still
 * single-tasking and not pre-empting. If you spend too much time in
 * this function, chaos will ensue. */
void wish_message_processor_task(wish_core_t* core, struct wish_event *e) {
    switch (e->event_type) {
    case WISH_EVENT_CONTINUE:
        WISHDEBUG(LOG_DEBUG,"Message processing started (continuation)\n\r");
        break;
    case WISH_EVENT_NEW_DATA:
        WISHDEBUG(LOG_DEBUG,"Message processing started (new data received)\n\r");
        break;
    case WISH_EVENT_NEW_CORE_CONNECTION:
        {
            /* Load the aliases of the connection partners from DB */
            wish_identity_t *tmp_id = wish_platform_malloc(sizeof (wish_identity_t));
            if (tmp_id == NULL) {
                WISHDEBUG(LOG_CRITICAL, "message processor task: Could not allocate memory");
                break;
            }
            int load_retval = wish_load_identity(e->context->local_wuid, tmp_id);
            char *local_alias = my_strdup(tmp_id->alias);
            int load_retval2 = wish_load_identity(e->context->remote_wuid, 
                        tmp_id);
            char *remote_alias = my_strdup(tmp_id->alias);
            if (load_retval != 1 || load_retval2 != 1) {
                WISHDEBUG(LOG_CRITICAL, "Unexpected problem with id db!");
            }

            if (local_alias != NULL && remote_alias != NULL) {
                    WISHDEBUG(LOG_CRITICAL ,"Connection established: %s > %s",
                        local_alias, remote_alias);

                wish_platform_free(tmp_id);
                wish_platform_free(local_alias);
                wish_platform_free(remote_alias);
            }

            e->context->context_state = WISH_CONTEXT_CONNECTED;
        }
        break;
    default:
        break;
    }

    wish_context_t* wish_handle = e->context;

    switch (e->event_type) {
    case WISH_EVENT_CONTINUE:
    case WISH_EVENT_NEW_DATA:
        wish_core_process_data(core, wish_handle);
        break;
    case WISH_EVENT_NEW_CORE_CONNECTION:
        wish_core_send_peers_rpc_req(core, e->context);
        break;
    case WISH_EVENT_FRIEND_REQUEST:
        WISHDEBUG(LOG_CRITICAL, "Accepting friend request");
        struct wish_event new_evt = {
            .event_type = WISH_EVENT_ACCEPT_FRIEND_REQUEST,
            .context = e->context,
        };
        wish_message_processor_notify(&new_evt);
        break;
    case WISH_EVENT_ACCEPT_FRIEND_REQUEST:
        if (e->context->curr_protocol_state 
                == PROTO_SERVER_STATE_READ_FRIEND_CERT) {
            e->context->curr_protocol_state 
                = PROTO_SERVER_STATE_REPLY_FRIEND_REQ;
            wish_core_handle_payload(core, e->context, NULL, 0);
        }
        else {
            WISHDEBUG(LOG_CRITICAL, "Unexpected state, closing connection!");
            wish_close_connection(core, e->context);
        }

        break;
    case WISH_EVENT_REQUEST_CONNECTION_CLOSING:
        wish_close_connection(core, e->context);
        break;
    default:
        WISHDEBUG(LOG_CRITICAL,"Uknown event type %d\n\r", e->event_type);
        break;
 
    }
}


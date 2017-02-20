#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "wish_rpc.h"
#include "wish_version.h"
#include "wish_identity.h"
#include "wish_io.h"
#include "wish_core_app_rpc_func.h"
#include "wish_core.h"
#include "wish_service_registry.h"
#include "core_service_ipc.h"
#include "wish_local_discovery.h"
#include "wish_connection_mgr.h"
#include "wish_dispatcher.h"
#include "ed25519.h"
#include "bson.h"
#include "cbson.h"
#include "bson_visitor.h"
#include "utlist.h"

#include "wish_debug.h"
#include "wish_port_config.h"

/*
#ifdef WISH_RPC_SERVER_STATIC_REQUEST_POOL
#define REQUEST_POOL_SIZE 10
static struct wish_rpc_context_list_elem request_pool[REQUEST_POOL_SIZE];
#endif

wish_rpc_server_t core_app_rpc_server = { 
    .server_name = "core/app",
#ifdef WISH_RPC_SERVER_STATIC_REQUEST_POOL
    .rpc_ctx_pool = request_pool,
    .rpc_ctx_pool_num_slots = REQUEST_POOL_SIZE,
#endif
};
*/

/* FIXME each Wish connection must have its own RCP client, so this has to be moved to ctx */
wish_rpc_client_t core2remote_rpc_client;

void write_bson_error(wish_rpc_ctx* ctx, int errno, char *errmsg);

// NBUFL and nbuf are used for writing BSON array indexes
#define NBUFL 8
uint8_t nbuf[NBUFL];


/* This is the Call-back functon invoksed by the core's "app" RPC
 * server, when identity.export is received from a Wish app 
 *
 * identity.export('342ef67c822662174e67689b8b1f1ef761c8085129561372adeb9ccf6ec30c86')
 * RPC app to core { op: 'methods',
 *   args: [],
 *   id: 3 }
 * Core to app: { ack: 3,
 *       data:
 *           {  }
 *       }
 *
 */
static void methods(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    struct wish_rpc_server_handler *h = core->core_app_rpc_server->list_head;
    
    bson bs; 
    bson_init(&bs);
    bson_append_start_object(&bs, "data");
    
    while (h != NULL) {
        bson_append_start_object(&bs, h->op_str);
        bson_append_finish_object(&bs);

        h = h->next;
    }
    
    bson_append_finish_object(&bs);
    bson_finish(&bs);
    
    wish_rpc_server_send(req, bs.data, bson_size(&bs));
    bson_destroy(&bs);
}

static void version(wish_rpc_ctx* req, uint8_t* args) {
    
    bson bs; 
    bson_init(&bs);
    bson_append_string(&bs, "data", WISH_CORE_VERSION_STRING);
    bson_finish(&bs);
    
    wish_rpc_server_send(req, bs.data, bson_size(&bs));
    bson_destroy(&bs);
}

static void services_send(wish_rpc_ctx* req, uint8_t* args) {
    //bson_visit("Handling services.send", args);
    
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    /* First, obtain the peer, as element "0" of args. This will
     * define the routing of the message. */
    uint8_t* peer = NULL;
    int32_t peer_len = 0;
    if (bson_get_document(args, "0", &peer, &peer_len) == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get peer");
        return;
    }

    /* Examine the 'rhid' element of the peer. If it matches our own
     * host's rhid, then it for local delivery, and pass it directly to 
     * "send core to app" function. We use the 'core app' RPC client for
     * this. */
    uint8_t *rhid = NULL;
    int32_t rhid_len = 0;
    if (bson_get_binary(peer, "rhid", &rhid, &rhid_len) == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get rhid");
        return;
    }

    //if (memcmp(rhid, this_host_rhid, WISH_WHID_LEN) == 0) { ... }
    
    /* Else, find a wish context that has a matching rhid.
     * Then, verify that luid and ruid also match the connection. If so,
     * then we can send the payload using with that context.
     * For this, use the 'core' RPC client. */
    uint8_t *luid = NULL;
    int32_t luid_len = 0;
    if (bson_get_binary(peer, "luid", &luid, &luid_len) == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get luid");
        return;
    }

    uint8_t *ruid = NULL;
    int32_t ruid_len = 0;
    if (bson_get_binary(peer, "ruid", &ruid, &ruid_len) == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get ruid");
        return;
    }

    uint8_t *rsid = NULL;
    int32_t rsid_len = 0;
    if (bson_get_binary(peer, "rsid", &rsid, &rsid_len) == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get rsid");
        return;
    }
    char *protocol = NULL;
    int32_t protocol_len = 0;
    if (bson_get_string(peer, "protocol", &protocol, &protocol_len) == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get protocol");
        return;
    }




    uint8_t *payload = NULL;
    int32_t payload_len = 0;
    if (bson_get_binary(args, "1", &payload, &payload_len) 
            == BSON_FAIL) {
        WISHDEBUG(LOG_CRITICAL, "Could not get payload");
        return;
    }

    /* First, check if message is to be delivered to some of our local services.
     * In this case we very if the message's rhid corresponds to our local core's rhid 
     */
    uint8_t local_hostid[WISH_WHID_LEN];
    wish_core_get_local_hostid(core, local_hostid);
    if (memcmp(rhid, local_hostid, WISH_WHID_LEN) == 0) {
        /* rhid matches to local core, message destined to a local service!
         * Now we must construct a frame, much like we do in the "core-to-core" 
         * RPC server, but in the peer document, the luid and ruid switch places,
         * and rsid is replaced by the service id which called this RPC handler
         * (that is found in the rpc context)
         *  */
        
        /* FIXME this is waisting stack space again */
        size_t upcall_doc_max_len = peer_len + payload_len + 100;
        uint8_t upcall_doc[upcall_doc_max_len];
        bson bs;
        bson_init_buffer(&bs, upcall_doc, upcall_doc_max_len);
        bson_append_string(&bs, "type", "frame");
        bson_append_start_object(&bs, "peer");
        /* luid and ruid switch places */
        bson_append_binary(&bs, "luid", ruid, WISH_ID_LEN);
        bson_append_binary(&bs, "ruid", luid, WISH_ID_LEN);
        bson_append_binary(&bs, "rhid", rhid, WISH_WHID_LEN);
        /* rsid is */
        bson_append_binary(&bs, "rsid", req->local_wsid, WISH_WSID_LEN);
        bson_append_string(&bs, "protocol", protocol);
        bson_append_finish_object(&bs);
        bson_append_binary(&bs, "data", payload, payload_len);
        bson_finish(&bs);
        if (bs.err) {
            WISHDEBUG(LOG_CRITICAL, "Error creating frame to local service");
        } else {
            //bson_visit("About to send this to local service on local core:", upcall_doc);
            send_core_to_app(core, rsid, upcall_doc, bson_get_doc_len(upcall_doc));
        }
        return;
    }
    /* Destination is determined to be a remote service on a remote core. */
    wish_context_t *dst_ctx = wish_core_lookup_ctx_by_luid_ruid_rhid(core, luid, ruid, rhid);

    /* Build the actual on-wire message:
     *
     * req: {
     *  op: 'send'
     *  args: [ lsid, rsid, protocol, payload ]
     * }
     */
    
    size_t args_buffer_len = 2*(WISH_WSID_LEN) + protocol_len + payload_len + 128;
    uint8_t args_buffer[args_buffer_len];
    bson bs; 
    bson_init_buffer(&bs, args_buffer, args_buffer_len);
    bson_append_start_array(&bs, "args");
    bson_append_binary(&bs, "0", req->local_wsid, WISH_WSID_LEN);
    bson_append_binary(&bs, "1", rsid, WISH_WSID_LEN);
    bson_append_string(&bs, "2", protocol);
    bson_append_binary(&bs, "3", payload, payload_len);
    bson_append_finish_array(&bs);
    bson_finish(&bs);
    
    if (bs.err) {
        WISHDEBUG(LOG_CRITICAL, "BSON write error, args_buffer");
        return;
    }

    size_t client_req_len = args_buffer_len + MAX_RPC_OP_LEN + 128;
    uint8_t client_req[client_req_len];
    
    wish_rpc_client_bson(&core2remote_rpc_client, "send", (char*)bson_data(&bs), bson_size(&bs), NULL, client_req, client_req_len);

    //bson_visit("About to send this to the remote core (should be req: { op, args, id }):", client_req);

    
    //WISHDEBUG(LOG_CRITICAL, "Sending services.send");
    if (dst_ctx != NULL && dst_ctx->context_state == WISH_CONTEXT_CONNECTED) {
        
        size_t req_len = client_req_len + 128;
        uint8_t req_buf[req_len];

        bson_iterator it;
        bson_find_from_buffer(&it, client_req, "op");
        const char* op = bson_iterator_string(&it);
        
        bool has_id = false;
        bson_find_from_buffer(&it, client_req, "id");
        if(bson_iterator_type(&it) == BSON_INT) {
            // we have an id
            has_id = true;
        }
        int id = bson_iterator_int(&it);
        
        bson_find_from_buffer(&it, client_req, "args");
        
        bson b;
        bson_init_buffer(&b, req_buf, req_len);
        bson_append_start_object(&b, "req");
        bson_append_string(&b, "op", op);
        bson_append_element(&b, "args", &it);
        if (has_id == true) { bson_append_int(&b, "id", id); }
        bson_append_finish_object(&b);
        bson_finish(&b);
        
        //bson_visit("About to send this to the remote core (should be req: { op, args[, id] }):", req_buf);
        
        
        int send_ret = wish_core_send_message(core, dst_ctx, req_buf, bson_get_doc_len(req_buf));
        if (send_ret != 0) {
            /* Sending failed. Propagate RPC error */
            WISHDEBUG(LOG_CRITICAL, "Core app RPC: Sending not possible at this time");
            if(req->id != 0) {
                wish_rpc_server_error(req, 506, "Failed sending message to remote core.");
            }
        }
        else {
            /* Sending successful */
        
            if(req->id != 0) {
                // Client expecting response. Send ack to client
                wish_rpc_server_send(req, NULL, 0);
            } else {
                /* Client not expecting response */
                wish_rpc_server_delete_rpc_ctx(req);
            }
        }
    }
    else {
        WISHDEBUG(LOG_CRITICAL, "Could not find a suitable wish context to send with");
        wish_debug_print_array(LOG_DEBUG, "should be luid:", luid, WISH_ID_LEN);
        wish_debug_print_array(LOG_DEBUG, "should be ruid:", ruid, WISH_ID_LEN);
        wish_debug_print_array(LOG_DEBUG, "should be rhid:", rhid, WISH_ID_LEN);
    }
}

static void services_list_handler(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = 300;
    uint8_t buffer[buffer_len];
    WISHDEBUG(LOG_CRITICAL, "Handling services.list buffer_len: %d", buffer_len);

    bson bs;
    
    bson_init_buffer(&bs, buffer, buffer_len);

    bson_append_start_object(&bs, "data");
    bson_append_string(&bs, "gurka", "a");
    bson_append_finish_object(&bs);
    bson_append_int(&bs, "ack", req->id);
    bson_finish(&bs);
    
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

/* This is the Call-back functon invoksed by the core's "app" RPC
 * server, when identity.export is received from a Wish app 
 *
 * identity.export('342ef67c822662174e67689b8b1f1ef761c8085129561372adeb9ccf6ec30c86')
 * RPC app to core { op: 'identity.export',
 *   args: [
 *   '342ef67c822662174e67689b8b1f1ef761c8085129561372adeb9ccf6ec30c86'
 *   ],
 *   id: 3 }
 * Core to app: { ack: 3,
 *       data:
 *       'H4sIAAAAAAAAA61TPW8...2d2b3GF2jAfwBaWrGAmsEAAA='
 *       }
 *
 */
static void identity_export_handler(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];

    WISHDEBUG(LOG_DEBUG, "Core app RPC: identity_export");
    bson_init_doc(buffer, buffer_len);

    /* Get the uid of identity to export, the uid is argument "0" in
     * args */
    uint8_t *arg_uid = 0;
    int32_t arg_uid_len = 0;
    if (bson_get_binary(args, "0", &arg_uid, &arg_uid_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument: uid");
        wish_rpc_server_error(req, 8, "Missing export uid argument");
        return;
    }

    if (arg_uid_len != WISH_ID_LEN) {
        WISHDEBUG(LOG_CRITICAL, "argument uid has illegal length");
        return;
    }

    /* Get the requested export type, element 1 of args array. 
     * This impelementation requires an
     * explicit string argument 'binary', which means that the
     * identity shall be exported as BSON document. */
    char *export_type_str = 0;
    int32_t export_type_str_len = 0;
    if (bson_get_string(args, "1", &export_type_str, &export_type_str_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Missing export type argument");
        wish_rpc_server_error(req, 8, "Missing export type argument");
        return;
    }

    if (strcmp(export_type_str, "binary") != 0) {
        WISHDEBUG(LOG_CRITICAL, "Illegal export type");
        return;
    }

    size_t id_bson_doc_max_len = sizeof (wish_identity_t) + 100;
    uint8_t id_bson_doc_unfiltered[id_bson_doc_max_len];
    int ret = wish_load_identity_bson(arg_uid, id_bson_doc_unfiltered, id_bson_doc_max_len);

    //bson_visit("Here's the source document while exporting...:", id_bson_doc_unfiltered);
    
    if (ret == 1) {
        uint8_t id_bson_doc[id_bson_doc_max_len];
        if (bson_filter_out_elem("privkey", id_bson_doc_unfiltered, id_bson_doc) == BSON_FAIL) {
            // FIXME This implementation is not easy to read. Also it exposes 
            //       everything except privkey, instead of exporting specific 
            //       elements. Rewrite using new bson lib.
            
            /* Encode the BSON id doc as elemet in 'data' array */
            if (bson_write_binary(buffer, buffer_len, "data", id_bson_doc_unfiltered, bson_get_doc_len(id_bson_doc_unfiltered)) != BSON_SUCCESS) {
                WISHDEBUG(LOG_CRITICAL, "Failed to encode identity as binary");
                wish_rpc_server_error(req, 341, "Failed to encode identity as binary A");
                return;
            }
        } else {
            /* Encode the BSON id doc as elemet in 'data' array */
            if (bson_write_binary(buffer, buffer_len, "data", id_bson_doc, bson_get_doc_len(id_bson_doc)) != BSON_SUCCESS) {
                WISHDEBUG(LOG_CRITICAL, "Failed to encode identity as binary");
                wish_rpc_server_error(req, 342, "Failed to encode identity as binary B");
                return;
            }
        }
        
        wish_rpc_server_send(req, buffer, bson_get_doc_len(buffer));
    } else {
        wish_rpc_server_error(req, 343, "Failed to load identity.");
    }
}

/* This is the Call-back functon invoksed by the core's "app" RPC
 * server, when identity.import is received from a Wish app 
 *
 *
 * Request:
 * identity.import(Buffer<the identity document as binary>,
 * Buffer<contactForWuid>, 'binary')
 * RPC app to core 
 * { op: 'identity.import',
 *  args: [ 
 *   <buffer 342ef... (The identity BSON document in a binary *   buffer)>,
 *   <buffer 342ef67c822662174e67689b8b1f1ef761c8085129561372adeb9ccf6ec30c86>,
 *   'binary'
 *   ],
 *   id: 3 }
 *
 * The second argument is the "befriend with" argument
 *
 * Core to app Reply:
 * { ack: 3,
 *   data: { alias: 'Stina', uid: <Buffer 04735247b938c585df04d4fbaab68a1f766f00195839a36087eaa1299c491449> } 
 *   } 
 *
 *   The first arg is alias and second argument is the uid of the imported id
 *
 */
static void identity_import_handler(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];

    WISHDEBUG(LOG_DEBUG, "Core app RPC: identity_import");
    bson_init_doc(buffer, buffer_len);

    /* Get the identity document to import, the doc is argument "0" in
     * args */
    uint8_t *new_id_doc = 0;
    int32_t new_id_doc_len = 0;
    if (bson_get_binary(args, "0", &new_id_doc, &new_id_doc_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument 0: id doc");
        wish_rpc_server_error(req, 70, "Could not get argument 0: identity doc");
        return;
    }

    uint8_t *befriend_wuid = 0;
    int32_t befriend_wuid_len = 0;
    if (bson_get_binary(args, "1", &befriend_wuid, &befriend_wuid_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument 1: befriend wuid");
        wish_rpc_server_error(req, 71, "Could not get argument 1: uid");
        return;
    }

    if (befriend_wuid_len != WISH_ID_LEN) {
        WISHDEBUG(LOG_CRITICAL, "argument 1 befriend wuid has illegal length");
        wish_rpc_server_error(req, 72, "Could not get argument 1: uid length not expected");
        return;
    }
    /* FIXME We don't have the concept of "befriend uid with new
     * contact */
 
    /* Get the requested import type, element 2 of args array. 
     * This impelementation requires an
     * explicit string argument 'binary', which means that the
     * identity is imported as BSON document (inside a BSON binary element). */
    char *import_type_str = 0;
    int32_t import_type_str_len = 0;
    if (bson_get_string(args, "2", &import_type_str, 
            &import_type_str_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Missing import type argument");
        wish_rpc_server_error(req, 73, "Missing import type argument");
        return;
    }

    if (strcmp(import_type_str, "binary") != 0) {
        WISHDEBUG(LOG_CRITICAL, "Illegal import type");
        wish_rpc_server_error(req, 74, "Illegal import type");
        return;
    }

    /* Start to examine the imported identity doc */
    
    int32_t bson_doc_len = bson_get_doc_len(new_id_doc);
    if (bson_get_doc_len(new_id_doc) != new_id_doc_len) {
        WISHDEBUG(LOG_CRITICAL, "Malformed doc, len %d", bson_doc_len);
        wish_rpc_server_error(req, 75, "Malformed bson document.");
        return;
    }
    
    /* FIXME make other sanity checks... like the existence of different
     * elements: pubkey, alias, ... */

    char *new_id_alias = NULL;
    int32_t new_id_alias_len = 0;
    if (bson_get_string(new_id_doc, "alias", &new_id_alias, &new_id_alias_len) 
            != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get alias element");
        wish_rpc_server_error(req, 76, "Could not get alias element");
        return;
    }
    else {
        WISHDEBUG(LOG_CRITICAL, "Importing alias %s", new_id_alias);
    }

    wish_identity_t new_id;
    memset(&new_id, 0, sizeof (wish_identity_t));
    if (wish_populate_id_from_cert(&new_id, new_id_doc)) {
        /* ...it failed somehow.. */
        WISHDEBUG(LOG_CRITICAL, "There was an error when populating the new id struct");
        wish_rpc_server_error(req, 76, "There was an error when populating the new id struct");
        return;
    }

    if (wish_identity_exists(new_id.uid)>0) {
        // it already exists, bail!
        wish_rpc_server_error(req, 202, "Identity already exists.");
        return;
    }
    
    /* The identity to be imported seems valid! */

    /* Save the new identity to database */
    //wish_save_identity_entry_bson(new_id_doc);
    int ret = wish_save_identity_entry(&new_id);
    
    if( ret != 0 ) {
        return write_bson_error(req, 201, "Too many identities.");
    }

    /* Form the reply message in 'buffer' */

    int32_t data_doc_max_len = WISH_ID_LEN + 20 + WISH_MAX_ALIAS_LEN + 20;
    uint8_t data_doc[data_doc_max_len];
    bson_init_doc(data_doc, data_doc_max_len);
    
    if (bson_write_string(data_doc, data_doc_max_len, "alias", new_id_alias)
            != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Failed to add alias to data doc");
        wish_rpc_server_error(req, 76, "Failed to add alias to data doc");
        return;
    }

    if (bson_write_binary(data_doc, data_doc_max_len, "uid", new_id.uid, WISH_ID_LEN) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Failed to to add id to data doc");
        wish_rpc_server_error(req, 76, "Failed to to add id to data doc");
        return;
    }
    
    if (bson_write_embedded_doc_or_array(buffer, buffer_len, 
            "data", data_doc, BSON_KEY_DOCUMENT) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Failed to to add data doc to response");
        wish_rpc_server_error(req, 76, "Failed to to add data doc to response");
        return;
    }

    wish_rpc_server_send(req, buffer, bson_get_doc_len(buffer));
}

/* This is the Call-back functon invoksed by the core's "app" RPC
 * server, when identity.list is received from a Wish app 
 *
 *  identity.list()
 *  RPC app to core { op: 'identity.list', args: [], id: 2 }
 *  Core to app: { ack: 2,
 *    data: 
 *       [ { alias: 'Jan2',
 *           id: '342ef67c822662174e67689b8b1f1ef761c8085129561372adeb9ccf6ec30c86',
 *           pubkey:'62d5b302ef33ee27bb52781b1b3946b04f856e5cf964f6418770e859338268f7',
 *           privkey: true,
 *           hosts: [Object],
 *           contacts: [Object],
 *           transports: [Object],
 *           trust: null },
 *
 *       ]
 *
 */
static void identity_list_handler(wish_rpc_ctx* req, uint8_t* args) {
    
    int num_uids_in_db = wish_get_num_uid_entries();
    wish_uid_list_elem_t uid_list[num_uids_in_db];
    int num_uids = wish_load_uid_list(uid_list, num_uids_in_db);

    bson bs; 
    bson_init(&bs);
    bson_append_start_array(&bs, "data");
    
    int i = 0;
    for (i = 0; i < num_uids; i++) {
        char num_str[8];
        bson_numstr(num_str, i);
        bson_append_start_object(&bs, num_str);
        /* For each uid in DB, copy the uid, alias and privkey fields */
        size_t id_bson_doc_max_len = sizeof (wish_identity_t) + 100;
        uint8_t id_bson_doc[id_bson_doc_max_len];
        int ret = wish_load_identity_bson(uid_list[i].uid, id_bson_doc,
            id_bson_doc_max_len);

        if (ret == 1) {

            int32_t uid_len = 0;
            uint8_t *uid = NULL;
            if (bson_get_binary(id_bson_doc, "uid", &uid, &uid_len) 
                    == BSON_FAIL) {
                WISHDEBUG(LOG_CRITICAL, "Unexpected: no uid");
                break;
            }
            bson_append_binary(&bs, "uid", uid, uid_len);

            int32_t alias_len = 0;
            char *alias = NULL;
            if (bson_get_string(id_bson_doc, "alias", &alias, &alias_len)
                    == BSON_FAIL) {
                WISHDEBUG(LOG_CRITICAL, "Unexpected: no alias");
                break;
            }
            bson_append_string(&bs, "alias", alias);

            int32_t privkey_len = 0;
            uint8_t *privkey = NULL;
            bool privkey_status = false;
            if (bson_get_binary(id_bson_doc, "privkey", &privkey,
                    &privkey_len) == BSON_SUCCESS) {
                /* Privkey exists in database */
                privkey_status = true;
            }
            else {
                /* Privkey not in database */
                privkey_status = false;
            }
            bson_append_bool(&bs, "privkey", privkey_status);

            bson_append_finish_object(&bs);
        }
    }
    
    bson_append_finish_array(&bs);
    bson_finish(&bs);
    
    if (bs.err) {
        WISHDEBUG(LOG_CRITICAL, "BSON error in identity_list_handler");
        wish_rpc_server_error(req, 997, "BSON error in identity_list_handler");
    } else {

        //bson_visit("identity.list response bson", bs.data);

        wish_rpc_server_send(req, bs.data, bson_size(&bs));
    }
    bson_destroy(&bs);
}

/*
 * identity.create
 *
 * App to core: { op: "identity.create", args: [ "Moster Greta" ], id: 5 }
 * Response core to App:
 *  { ack: 5, data: {
 *          alias: "Moster Greta",
 *          uid: <binary buffer containing the new wish user id>,
 *          privkey: true;
 *      }
 *  }
 *
 *  Note that privkey is always returned as 'true' when doing
 *  identity.create (An identity creation always involves creation of
 *  private key and public key)
 */
static void identity_create_handler(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];

    bson_init_doc(buffer, buffer_len);

    /* Get the new identity's alias, it is element 0 of array 'args' */
    char *alias_str = NULL;
    int32_t alias_str_len = 0;
    bson_get_string(args, "0", &alias_str, &alias_str_len);

    WISHDEBUG(LOG_DEBUG, "Core app RPC: identity_create for alias %s",
        alias_str);

    /* Create the identity */
    wish_identity_t new_id;
    wish_create_local_identity(&new_id, alias_str);
    int ret = wish_save_identity_entry(&new_id);

    /* There is something wrong with this. Returns error while saving. Should be <= 0?
    if( ret != 0 ) {
        return write_bson_error(req, req_id, 201, "Too many identities.");
    }
    */

    size_t data_doc_max_len = sizeof (wish_identity_t) + 100;
    uint8_t data_doc[data_doc_max_len];
    ret = wish_load_identity_bson(new_id.uid, data_doc, data_doc_max_len);
    if (ret == 1) {
        /* Filter out the actual "privkey" element */
        size_t filtered_doc_max_len = bson_get_doc_len(data_doc);
        uint8_t filtered_doc[filtered_doc_max_len];
        if (bson_filter_out_elem("privkey", data_doc, filtered_doc)
                == BSON_FAIL) {
            WISHDEBUG(LOG_CRITICAL, "Could not filter out privkey!");
        }
        else {
            if (bson_write_boolean(filtered_doc, filtered_doc_max_len, 
                    "privkey", true) == BSON_FAIL) {
                WISHDEBUG(LOG_CRITICAL, "Could not add bool privkey!");
            }
        }

        bson_write_embedded_doc_or_array(buffer, buffer_len, 
            "data", filtered_doc, BSON_KEY_DOCUMENT);
    }

    wish_rpc_server_send(req, buffer, bson_get_doc_len(buffer));

    WISHDEBUG(LOG_CRITICAL, "Starting to advertize the new identity");
    wish_core_update_identities(core);

    wish_ldiscover_advertize(core, new_id.uid);
    wish_report_identity_to_local_services(core, &new_id, true);
}
/*
 * identity.remove
 *
 * App to core: { op: "identity.remove", args: [ <Buffer> uid ], id: 5 }
 * Response core to App:
 *  { ack: 5, data: true }
 *
 *  Note that privkey is always returned as 'true' when doing
 *  identity.create (An identity creation always involves creation of
 *  private key and public key)
 */
static void identity_remove_handler(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];

    bson_iterator it;
    bson_find_from_buffer(&it, args, "0");

    if(bson_iterator_type(&it) == BSON_BINDATA && bson_iterator_bin_len(&it) == WISH_ID_LEN) {

        /* Get the uid of identity to export, the uid is argument "0" in
         * args */
        uint8_t *luid = 0;
        luid = (uint8_t *)bson_iterator_bin_data(&it);

        wish_identity_t id_to_remove;
        if (wish_load_identity(luid, &id_to_remove) > 0) {
            wish_report_identity_to_local_services(core, &id_to_remove, false);
        }
        
        int res = wish_identity_remove(core, luid);

        bson bs;

        bson_init_buffer(&bs, buffer, buffer_len);
        bson_append_bool(&bs, "data", res == 1 ? true : false);
        bson_finish(&bs);

        if (bs.err != 0) {
            return write_bson_error(req, 344, "Failed writing reponse.");
        }

        wish_core_update_identities(core);
        wish_rpc_server_send(req, buffer, bson_get_doc_len(buffer));
    } else {
        write_bson_error(req, 343, "Invalid argument. Expecting 32 byte bin data.");
    }
}

/*
 * identity.sign
 *
 * App to core: { op: "identity.sign", args: [ <Buffer> uid, <Buffer> hash ], id: 5 }
 */
static void identity_sign(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];

    bson_iterator it;
    bson_find_from_buffer(&it, args, "0");
    
    if(bson_iterator_type(&it) != BSON_BINDATA || bson_iterator_bin_len(&it) != WISH_ID_LEN) {
        return write_bson_error(req, 345, "Invalid uid.");
    }

    uint8_t *luid = 0;
    luid = (uint8_t *)bson_iterator_bin_data(&it);

    bson_find_from_buffer(&it, args, "1");
    
    if(bson_iterator_type(&it) != BSON_BINDATA || bson_iterator_bin_len(&it) < 32 || bson_iterator_bin_len(&it) > 64 ) {
        return write_bson_error(req, 345, "Invalid hash.");
    }
    
    char hash[64];
    int hash_len = bson_iterator_bin_len(&it);
    
    memcpy(hash, bson_iterator_bin_data(&it), hash_len);

    uint8_t signature[ED25519_SIGNATURE_LEN];
    uint8_t local_privkey[WISH_PRIVKEY_LEN];
    if (wish_load_privkey(luid, local_privkey)) {
        WISHDEBUG(LOG_CRITICAL, "Could not load privkey");
        return write_bson_error(req, 345, "Could not load private key.");
    }
    else {
        ed25519_sign(signature, hash, hash_len, local_privkey);
    }

    //wish_debug_print_array(LOG_DEBUG, signature, ED25519_SIGNATURE_LEN);

    bson bs;

    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_binary(&bs, "data", signature, ED25519_SIGNATURE_LEN);
    bson_finish(&bs);

    if(bs.err != 0) {
        return write_bson_error(req, 344, "Failed writing reponse.");
    }

    wish_rpc_server_send(req, buffer, bson_get_doc_len(buffer));
}

/*
 * identity.verify
 *
 * App to core: { op: "identity.verify", args: [ <Buffer> uid, <Buffer> signature, <Buffer> hash ], id: 5 }
 */
static void identity_verify(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];

    bson_iterator it;
    bson_find_from_buffer(&it, args, "0");
    
    if(bson_iterator_type(&it) != BSON_BINDATA || bson_iterator_bin_len(&it) != WISH_ID_LEN) {
        return write_bson_error(req, 345, "Invalid uid.");
    }

    uint8_t *luid = 0;
    luid = (uint8_t *)bson_iterator_bin_data(&it);

    bson_find_from_buffer(&it, args, "1");
    
    if(bson_iterator_type(&it) != BSON_BINDATA || bson_iterator_bin_len(&it) != 64 ) {
        return write_bson_error(req, 346, "Invalid signature.");
    }
    
    char signature[64];
    int signature_len = bson_iterator_bin_len(&it);
    
    memcpy(signature, bson_iterator_bin_data(&it), signature_len);

    bson_find_from_buffer(&it, args, "2");
    
    if(bson_iterator_type(&it) != BSON_BINDATA || bson_iterator_bin_len(&it) < 32 || bson_iterator_bin_len(&it) > 64 ) {
        return write_bson_error(req, 345, "Invalid hash.");
    }
    
    char hash[64];
    int hash_len = bson_iterator_bin_len(&it);
    
    memcpy(hash, bson_iterator_bin_data(&it), hash_len);

    bool verification = false;
    uint8_t pubkey[WISH_PUBKEY_LEN];
    if (wish_load_pubkey(luid, pubkey)) {
        WISHDEBUG(LOG_CRITICAL, "Could not load pubkey");
        return write_bson_error(req, 345, "Could not load private key.");
    } else {
        verification = ed25519_verify(signature, hash, hash_len, pubkey) ? true : false;
    }

    //wish_debug_print_array(LOG_DEBUG, signature, ED25519_SIGNATURE_LEN);

    bson bs;

    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_bool(&bs, "data", verification);
    bson_finish(&bs);

    if(bs.err != 0) {
        return write_bson_error(req, 344, "Failed writing reponse.");
    }

    wish_rpc_server_send(req, buffer, bson_get_doc_len(buffer));
}

/*
 * identity.get
 *
 * App to core: { op: "identity.get", args: [ Buffer(32) uid ], id: 5 }
 * Response core to App:
 *  { ack: 5, data: {
 *          alias: "Moster Greta",
 *          uid: <binary buffer containing the new wish user id>,
 *          privkey: true,
 *          pubkey: Buffer
 *      }
 *  }
 *
 *  Note that privkey is always returned as 'true' when doing
 *  identity.create (An identity creation always involves creation of
 *  private key and public key)
 */
static void identity_get_handler(wish_rpc_ctx* req, uint8_t* args) {
    WISHDEBUG(LOG_DEBUG, "In identity_get_handler");

    bson bs; 
    bson_init(&bs);
    bson_append_start_object(&bs, "data");
    
    /* Get the uid of identity to get, the uid is argument "0" in
     * args */
    uint8_t *arg_uid = 0;
    int32_t arg_uid_len = 0;
    if (bson_get_binary(args, "0", &arg_uid, &arg_uid_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument: uid");
        return;
    }

    if (arg_uid_len != WISH_ID_LEN) {
        WISHDEBUG(LOG_CRITICAL, "argument uid has illegal length");
        return;
    }


    int num_uids_in_db = wish_get_num_uid_entries();
    wish_uid_list_elem_t uid_list[num_uids_in_db];
    int num_uids = wish_load_uid_list(uid_list, num_uids_in_db);

    int i = 0;
    for (i = 0; i < num_uids; i++) {
        if( memcmp(uid_list[i].uid, arg_uid, 32) != 0 ) {
            continue;
        }
        
        /* For each uid in DB, copy the uid, alias and privkey fields */
        size_t id_bson_doc_max_len = sizeof (wish_identity_t) + 100;
        uint8_t id_bson_doc[id_bson_doc_max_len];
        int ret = wish_load_identity_bson(uid_list[i].uid, id_bson_doc,
            id_bson_doc_max_len);
        
        if (ret == 1) {

            int32_t uid_len = 0;
            uint8_t *uid = NULL;
            if (bson_get_binary(id_bson_doc, "uid", &uid, &uid_len) 
                    == BSON_FAIL) {
                WISHDEBUG(LOG_CRITICAL, "Unexpected: no uid");
                break;
            }
            bson_append_binary(&bs, "uid", uid, uid_len);

            int32_t alias_len = 0;
            char *alias = NULL;
            if (bson_get_string(id_bson_doc, "alias", &alias, &alias_len)
                    == BSON_FAIL) {
                WISHDEBUG(LOG_CRITICAL, "Unexpected: no alias");
                break;
            }
            bson_append_string(&bs, "alias", alias);

            int32_t privkey_len = 0;
            uint8_t *privkey = NULL;
            bool privkey_status = false;
            if (bson_get_binary(id_bson_doc, "privkey", &privkey,
                    &privkey_len) == BSON_SUCCESS) {
                /* Privkey exists in database */
                privkey_status = true;
            }
            else {
                /* Privkey not in database */
                privkey_status = false;
            }
            bson_append_bool(&bs, "privkey", privkey_status);
            
            int32_t pubkey_len = 0;
            uint8_t *pubkey = NULL;
            if (bson_get_binary(id_bson_doc, "pubkey", &pubkey, &pubkey_len) == BSON_FAIL) {
                WISHDEBUG(LOG_CRITICAL, "Error while reading pubkey");
                wish_rpc_server_error(req, 509, "This is fail!");
                break;
            }
            bson_append_binary(&bs, "pubkey", pubkey, pubkey_len);
        } else {
            WISHDEBUG(LOG_CRITICAL, "Could not load identity");
            wish_rpc_server_error(req, 997, "Could not load identity");
        }
        
        break;
    }
    
    if (i >= num_uids) {
        WISHDEBUG(LOG_CRITICAL, "The identity was not found");
        wish_rpc_server_error(req, 997, "The identity was not found");
    } else {
        bson_append_finish_object(&bs);
        bson_finish(&bs);

        if (bs.err) {
            WISHDEBUG(LOG_CRITICAL, "BSON error in identity_get_handler");
            wish_rpc_server_error(req, 997, "BSON error in identity_get_handler");
        } else {
            wish_rpc_server_send(req, bs.data, bson_size(&bs));
        }
    }
    bson_destroy(&bs);
}

static void connections_list_handler(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];
    
    wish_context_t *db = wish_core_get_connection_pool(core);
    
    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_start_array(&bs, "data");
    
    int i;
    int p = 0;
    for(i=0; i< WISH_CONTEXT_POOL_SZ; i++) {
        if(db[i].context_state != WISH_CONTEXT_FREE) {
            bson_numstrn((char *)nbuf, NBUFL, p);
            //bson_append_start_object(&bs, nbuf);
            bson_append_start_object(&bs, (char *)nbuf);
            bson_append_int(&bs, "cid", i);
            bson_append_binary(&bs, "luid", db[i].local_wuid, WISH_ID_LEN);
            bson_append_binary(&bs, "ruid", db[i].remote_wuid, WISH_ID_LEN);
            bson_append_binary(&bs, "rhid", db[i].remote_hostid, WISH_ID_LEN);
            //bson_append_bool(&bs, "online", true);
            bson_append_bool(&bs, "outgoing", db[i].outgoing);
            //bson_append_bool(&bs, "relay", db[i].via_relay);
            //bson_append_bool(&bs, "authenticated", true);
            /*
            bson_append_start_object(&bs, "transport");
            bson_append_string(&bs, "type", "tcp");
            bson_append_string(&bs, "localAddress", "5.5.5.5:5555");
            bson_append_string(&bs, "remoteAddress", "6.6.6.6:6666");
            bson_append_finish_object(&bs);
            */
            bson_append_finish_object(&bs);
            p++;
            if(p >= 4) { break; }
        }
    }

    bson_append_finish_array(&bs);
    bson_finish(&bs);
    
    if (bs.err) {
        write_bson_error(req, 303, "Failed writing bson.");
    }
    
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

static void connections_disconnect_handler(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    int buffer_len = 1400;
    uint8_t buffer[buffer_len];
    
    wish_context_t *db = wish_core_get_connection_pool(core);

    bson_iterator it;
    bson_find_from_buffer(&it, args, "0");
    
    if (bson_iterator_type(&it) == BSON_INT) {

        int idx = bson_iterator_int(&it);
        
        wish_close_connection(core, &db[idx]);

        bson bs;

        bson_init_buffer(&bs, buffer, buffer_len);
        bson_append_bool(&bs, "data", true);
        bson_finish(&bs);

        if(bs.err != 0) {
            write_bson_error(req, 344, "Failed writing reponse.");
        }
        
        wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
    } else {
        write_bson_error(req, 343, "Invalid argument. Int index.");
    }
}

/**
 *  
 */
static void connections_check_connections(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    int buffer_len = 1400;
    uint8_t buffer[buffer_len];
    
    wish_connections_check(core);
    
    bson bs;

    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_bool(&bs, "data", true);
    bson_finish(&bs);

    if(bs.err != 0) {
        write_bson_error(req, 344, "Failed writing reponse.");
    }
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

/*
 * Wish Local Discovery
 *
 * App to core: { op: "wld.list", args: [ <Buffer> uid ], id: 5 }
 * Response core to App:
 *  { ack: 5, data: [item] }
 *
 *  item: {
 *   ruid: Buffer<>,
 *   rhid: Buffer<>,
 *   [pubkey: Buffer<>] // optional
 * }
 * 
 */
static void wld_list_handler(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = WISH_PORT_RPC_BUFFER_SZ;
    uint8_t buffer[buffer_len];
    
    wish_ldiscover_t *db = wish_ldiscover_get();

    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_start_array(&bs, "data");
    
    int i;
    int p = 0;
    for(i=0; i< WISH_LOCAL_DISCOVERY_MAX; i++) {
        if(db[i].occupied) {
            bson_numstrn((char *)nbuf, NBUFL, p);
            //bson_append_start_object(&bs, nbuf);
            bson_append_start_object(&bs, (char *)nbuf);
            bson_append_string(&bs, "alias", db[i].alias);
            bson_append_binary(&bs, "ruid", db[i].ruid, WISH_ID_LEN);
            bson_append_binary(&bs, "rhid", db[i].rhid, WISH_ID_LEN);
            bson_append_binary(&bs, "pubkey", db[i].pubkey, WISH_PUBKEY_LEN);
            bson_append_finish_object(&bs);
            p++;
        }
    }

    bson_append_finish_array(&bs);
    bson_finish(&bs);
    
    if (bs.err) {
        write_bson_error(req, 303, "Failed writing bson.");
    }
    
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

/*
 * Wish Local Discovery
 *
 * App to core: { op: "wld.list", args: [ <Buffer> uid ], id: 5 }
 * Response core to App:
 *  { ack: 5, data: [item] }
 *
 *  item: {
 *   ruid: Buffer<>,
 *   rhid: Buffer<>,
 *   [pubkey: Buffer<>] // optional
 * }
 * 
 */
static void wld_clear_handler(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = 1400;
    uint8_t buffer[buffer_len];
    
    wish_ldiscover_clear();

    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_bool(&bs, "data", true);
    bson_finish(&bs);
    
    if (bs.err) {
        write_bson_error(req, 303, "Failed writing bson.");
    }
    
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

/*
 * Wish Local Discovery
 *
 * App to core: { op: "wld.list", args: [ <Buffer> uid ], id: 5 }
 * Response core to App:
 *  { ack: 5, data: [item] }
 *
 *  item: {
 *   ruid: Buffer<>,
 *   rhid: Buffer<>,
 *   [pubkey: Buffer<>] // optional
 * }
 * 
 */



static void wld_friend_request_handler(wish_rpc_ctx* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    int buffer_len = 1400;
    uint8_t buffer[buffer_len];

    /* Get the uid of identity to export, the uid is argument "0" in
     * args */
    uint8_t *luid = 0;
    int32_t luid_len = 0;
    if (bson_get_binary(args, "0", &luid, &luid_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument: luid");
        return;
    }

    if (luid_len != WISH_ID_LEN) {
        WISHDEBUG(LOG_CRITICAL, "argument luid has illegal length");
        return;
    }

    uint8_t *ruid = 0;
    int32_t ruid_len = 0;
    if (bson_get_binary(args, "1", &ruid, &ruid_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument: ruid");
        return;
    }

    if (ruid_len != WISH_ID_LEN) {
        WISHDEBUG(LOG_CRITICAL, "argument ruid has illegal length");
        return;
    }

    uint8_t *rhid = 0;
    int32_t rhid_len = 0;
    if (bson_get_binary(args, "2", &rhid, &rhid_len) != BSON_SUCCESS) {
        WISHDEBUG(LOG_CRITICAL, "Could not get argument: rhid");
        return;
    }

    if (rhid_len != WISH_ID_LEN) {
        WISHDEBUG(LOG_CRITICAL, "argument rhid has illegal length");
        return;
    }

    // now check if we have the wld details for this entry
    
    wish_ldiscover_t *db = wish_ldiscover_get();

    bool found = false;
    
    int i;
    for(i=0; i< WISH_LOCAL_DISCOVERY_MAX; i++) {
        if( db[i].occupied && 
                memcmp(&db[i].ruid, ruid, WISH_ID_LEN) == 0 &&
                memcmp(&db[i].rhid, rhid, WISH_ID_LEN) == 0) {
            WISHDEBUG(LOG_CRITICAL, "Found in slot %d", i);
            found = true;
            break;
        }
    }
    
    if(!found) {
        wish_rpc_server_error(req, 304, "Wld entry not found.");
        return;
    }

    wish_context_t *friend_req_ctx = wish_core_start(core, luid, ruid);
    friend_req_ctx->friend_req_connection = true;
    uint8_t *ip = db[i].transport_ip.addr;
    WISHDEBUG(LOG_CRITICAL, "Will start a friend req connection to: %u.%u.%u.%u\n", 
            ip[3], ip[2], ip[1], ip[0]);


    wish_open_connection(core, friend_req_ctx, &(db[i].transport_ip), db[i].transport_port, false);

    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_string(&bs, "data", "wait");
    bson_finish(&bs);
    
    if (bs.err) {
        write_bson_error(req, 303, "Failed writing bson.");
    }
    
    wish_rpc_server_emit(req, bson_data(&bs), bson_size(&bs));
}

static void host_config(wish_rpc_ctx* req, uint8_t* args) {
    int buffer_len = 1400;
    uint8_t buffer[buffer_len];
    
    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_start_object(&bs, "data");
    // FIXME version is shown in the separate version rpc command, consider removing this
    bson_append_string(&bs, "version", WISH_CORE_VERSION_STRING);
    bson_append_finish_object(&bs);
    bson_finish(&bs);

    if (bs.err) {
        write_bson_error(req, 305, "Failed writing bson.");
    }
    
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

/*
static void debug_enable(struct wish_rpc_context* req, 
                                uint8_t *args, wish_rpc_id_t req_id,
                                uint8_t *buffer, size_t buffer_len) {
    bool enable = true;
    bson_iterator it;
    bson_find_from_buffer(&it, args, "1");
    if (bson_iterator_type(&it) == BSON_BOOL) {
        if ( false == bson_iterator_bool(&it) ) {
            enable = false;
        }
    }
    bson_find_from_buffer(&it, args, "0");
    
    if (bson_iterator_type(&it) == BSON_INT) {

        int stream = bson_iterator_int(&it);
        
        wish_debug_set_stream(stream, enable);
        
        bson bs;

        bson_init_buffer(&bs, buffer, buffer_len);
        bson_append_bool(&bs, "data", true);
        bson_append_int(&bs, "ack", req_id);
        bson_finish(&bs);

        if(bs.err != 0) {
            write_bson_error(req, req_id, 344, "Failed writing reponse.");
        }
    } else {
        write_bson_error(req, req_id, 343, "Invalid argument. Int index.");
    }
}

static void debug_disable(struct wish_rpc_context* req, 
                                uint8_t *args, wish_rpc_id_t req_id,
                                uint8_t *buffer, size_t buffer_len) {
    bson_iterator it;
    bson_find_from_buffer(&it, args, "0");
    
    if (bson_iterator_type(&it) == BSON_INT) {

        int stream = bson_iterator_int(&it);
        
        wish_debug_set_stream(stream, false);
        
        bson bs;

        bson_init_buffer(&bs, buffer, buffer_len);
        bson_append_bool(&bs, "data", true);
        bson_append_int(&bs, "ack", req_id);
        bson_finish(&bs);

        if(bs.err != 0) {
            write_bson_error(req, req_id, 344, "Failed writing reponse.");
        }
    } else {
        write_bson_error(req, req_id, 343, "Invalid argument. Int index.");
    }
}
*/

void write_bson_error(wish_rpc_ctx* req, int errno, char *errmsg) {
    int buffer_len = 400;
    uint8_t buffer[buffer_len];

    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    
    size_t error_max_len = 100;
    uint8_t error[error_max_len];
    bson_init_doc(error, error_max_len);

    bson_append_start_object(&bs, "data");
    bson_append_int(&bs, "code", errno);
    bson_append_string(&bs, "msg", errmsg);
    bson_append_finish_object(&bs);

    bson_append_int(&bs, "err", req->id);
    
    wish_rpc_server_send(req, bson_data(&bs), bson_size(&bs));
}

struct wish_rpc_server_handler methods_handler =                { .op_str = "methods",                .handler = methods };
struct wish_rpc_server_handler version_handler =                { .op_str = "version",                .handler = version };
struct wish_rpc_server_handler services_send_handler =          { .op_str = "services.send",          .handler = services_send };
struct wish_rpc_server_handler identity_sign_handler =          { .op_str = "identity.sign",          .handler = identity_sign };
struct wish_rpc_server_handler identity_verify_handler =        { .op_str = "identity.verify",        .handler = identity_verify };
struct wish_rpc_server_handler host_config_handler =            { .op_str = "host.config",            .handler = host_config };

void wish_core_app_rpc_init(wish_core_t* core) {
    core->core_app_rpc_server = wish_platform_malloc(sizeof(wish_rpc_server_t));
    core->core_app_rpc_server->request_list_head = NULL;
    core->core_app_rpc_server->rpc_ctx_pool = wish_platform_malloc(sizeof(struct wish_rpc_context_list_elem)*10);
    core->core_app_rpc_server->rpc_ctx_pool_num_slots = 10;
    strncpy(core->core_app_rpc_server->server_name, "core-from-app", 16);
    core->core_app_rpc_server->context = core;
    
    wish_rpc_server_register(core->core_app_rpc_server, &methods_handler);
    wish_rpc_server_register(core->core_app_rpc_server, &version_handler);
    
    wish_rpc_server_register(core->core_app_rpc_server, &services_send_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "services.list", services_list_handler);
    
    wish_rpc_server_add_handler(core->core_app_rpc_server, "identity.list", identity_list_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "identity.export", identity_export_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "identity.import", identity_import_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "identity.create", identity_create_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "identity.get", identity_get_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "identity.remove", identity_remove_handler);
    wish_rpc_server_register(core->core_app_rpc_server, &identity_sign_handler);
    wish_rpc_server_register(core->core_app_rpc_server, &identity_verify_handler);
    
    wish_rpc_server_add_handler(core->core_app_rpc_server, "connections.list", connections_list_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "connections.disconnect", connections_disconnect_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "connections.checkConnections", connections_check_connections);
    
    wish_rpc_server_add_handler(core->core_app_rpc_server, "wld.list", wld_list_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "wld.clear", wld_clear_handler);
    wish_rpc_server_add_handler(core->core_app_rpc_server, "wld.friendRequest", wld_friend_request_handler);
    
    wish_rpc_server_register(core->core_app_rpc_server, &host_config_handler);
    
    //wish_rpc_server_add_handler(core->core_app_rpc_server, "debug.enable", debug_enable);
    //wish_rpc_server_add_handler(core->core_app_rpc_server, "debug.disable", debug_disable);
}

static void wish_core_app_rpc_send(void *ctx, uint8_t *data, int len) {
    struct wish_rpc_context *req = (struct wish_rpc_context*) ctx;

    uint8_t* wsid = req->local_wsid;
    wish_core_t* core = (wish_core_t*) req->server->context;
    
    send_core_to_app(core, wsid, data, len);
}

void wish_core_app_rpc_handle_req(wish_core_t* core, uint8_t src_wsid[WISH_ID_LEN], uint8_t *data) {
#if 0
    // enable this for debug output of all the apps requests to core
    bson_visit("wish_core_app_rpc_handle_req:", data);
#endif

    char *op_str = NULL;
    int32_t op_str_len = 0;
    bson_get_string(data, "op", &op_str, &op_str_len);

    uint8_t *args = NULL;
    int32_t args_len = 0;
    if (bson_get_array(data, "args", &args, &args_len) == BSON_FAIL) {
        WISHDEBUG(LOG_DEBUG, "Could not get args of incoming RPC message");
    }


    bool ack_required = false;
    int32_t id = 0;
    if (bson_get_int32(data, "id", &id)) {
        ack_required = true;
    }

#if 0
    struct wish_rpc_context req = {
        .server = core->core_app_rpc_server,
        .send = wish_core_app_rpc_send,
        .send_context = src_wsid,
        .op_str = op_str,
        .id = id,
        .local_wsid = src_wsid,
    };
#endif
    struct wish_rpc_context_list_elem *list_elem = wish_rpc_server_get_free_rpc_ctx_elem(core->core_app_rpc_server);
    if (list_elem == NULL) {
        WISHDEBUG(LOG_CRITICAL, "Core app RPC: Could not save the rpc context. Failing in wish_core_app_rpc_func.");
        return;
    } else {
        struct wish_rpc_context *rpc_ctx = &(list_elem->request_ctx);
        rpc_ctx->server = core->core_app_rpc_server;
        rpc_ctx->send = wish_core_app_rpc_send;
        rpc_ctx->send_context = rpc_ctx;
        memcpy(rpc_ctx->op_str, op_str, MAX_RPC_OP_LEN);
        rpc_ctx->id = id;
        rpc_ctx->local_wsid = src_wsid;
    
        if (wish_rpc_server_handle(core->core_app_rpc_server, rpc_ctx, args)) {
            WISHDEBUG(LOG_DEBUG, "RPC server fail: wish_core_app_rpc_func");
        }
    }
}

void wish_core_app_rpc_cleanup_requests(wish_core_t* core, uint8_t *wsid) {
    struct wish_rpc_context_list_elem *list_elem = NULL;
    struct wish_rpc_context_list_elem *tmp = NULL;
    LL_FOREACH_SAFE(core->core_app_rpc_server->request_list_head, list_elem, tmp) {
        if (memcmp(list_elem->request_ctx.local_wsid, wsid, WISH_WSID_LEN)) {
            WISHDEBUG(LOG_CRITICAL, "App rpc server clean up: request op %s", list_elem->request_ctx.op_str);
#ifdef WISH_RPC_SERVER_STATIC_REQUEST_POOL
            memset(&(list_elem->request_ctx), 0, sizeof(wish_rpc_ctx));
#else
#error not implemented
            //wish_platform_free....
#endif
            LL_DELETE(core->core_app_rpc_server->request_list_head, list_elem);
        }
    }
}

void wish_send_peer_update_locals(wish_core_t* core, uint8_t *dst_wsid, struct wish_service_entry *service_entry, bool online) {
    WISHDEBUG(LOG_DEBUG, "In update locals");
    if (memcmp(dst_wsid, service_entry->wsid, WISH_ID_LEN) == 0) {
        /* Don't send any peer online/offline messages regarding service itself */
        return;
    }
    
    wish_uid_list_elem_t local_id_list[WISH_NUM_LOCAL_IDS];
    int num_local_ids = wish_get_local_identity_list(local_id_list, WISH_NUM_LOCAL_IDS);
    if (num_local_ids == 0) {
        WISHDEBUG(LOG_CRITICAL, "Unexpected: no local identities");
        return;
    } else {
        WISHDEBUG(LOG_DEBUG, "Local id list: %i", num_local_ids);
    }
    
    uint8_t local_hostid[WISH_WHID_LEN];
    wish_core_get_local_hostid(core, local_hostid);
            
            
    int i = 0;
    int j = 0;
    for (i = 0; i < num_local_ids; i++) {
        for (j = 0; j < num_local_ids; j++) {
            bson bs;
            int buffer_len = 2 * WISH_ID_LEN + WISH_WSID_LEN + WISH_WHID_LEN + WISH_PROTOCOL_NAME_MAX_LEN + 200;
            uint8_t buffer[buffer_len];
            bson_init_buffer(&bs, buffer, buffer_len);
            
            bson_append_string(&bs, "type", "peer");
            bson_append_start_object(&bs, "peer");
            bson_append_binary(&bs, "luid", (uint8_t*) local_id_list[i].uid, WISH_ID_LEN);
            bson_append_binary(&bs, "ruid", (uint8_t*) local_id_list[j].uid, WISH_ID_LEN);
            bson_append_binary(&bs, "rsid", (uint8_t*) service_entry->wsid, WISH_WSID_LEN);
            bson_append_binary(&bs, "rhid", (uint8_t*) local_hostid, WISH_ID_LEN);
            /* FIXME support more protocols than just one */
            bson_append_string(&bs, "protocol", &(service_entry->protocols[0][0]));   
            bson_append_string(&bs, "type", "N");
            bson_append_bool(&bs, "online", online);
            bson_append_finish_object(&bs);
           
            bson_finish(&bs);
            if (bs.err) {
                WISHDEBUG(LOG_CRITICAL, "BSON error when creating peer message: %i %s len %i", bs.err, bs.errstr, bs.dataSize);
            }
            else {
                send_core_to_app(core, dst_wsid, (uint8_t *) bson_data(&bs), bson_size(&bs));
            }
        }
    }
}

/** Report the existence of the new identity to local services:
 *
 * Let the new identity to be i.
 * Let the local host identity to be h.
 * For every service "s" present in the local service registry, do;
 *    For every service "r" present in the local service registry, do:
 *      Construct "type: peer", "online: true", message with: <luid=i, ruid=i, rsid=r, rhid=h> and send it to s. If r == s, skip to avoid sending online message to service itself.
 *    done
 * done.      
 * 
 * @param identity the identity to send updates for
 * @param online true, if the identity is online (e.g. true when identity is created, false when identity is deleted)
 */
void wish_report_identity_to_local_services(wish_core_t* core, wish_identity_t* identity, bool online) {
    uint8_t local_hostid[WISH_WHID_LEN];
    wish_core_get_local_hostid(core, local_hostid);
    struct wish_service_entry *service_registry = wish_service_get_registry(core);
    int i = 0;
    for (i = 0; i < WISH_MAX_SERVICES; i++) {
        if (wish_service_entry_is_valid(core, &(service_registry[i]))) {
            int j = 0;
            for (j = 0; j < WISH_MAX_SERVICES; j++) {
                if (wish_service_entry_is_valid(core, &(service_registry[j]))) {
                    if (memcmp(service_registry[i].wsid, service_registry[j].wsid, WISH_WSID_LEN) != 0) {
                        bson bs;
                        int buffer_len = 2 * WISH_ID_LEN + WISH_WSID_LEN + WISH_WHID_LEN + WISH_PROTOCOL_NAME_MAX_LEN + 200;
                        uint8_t buffer[buffer_len];
                        bson_init_buffer(&bs, buffer, buffer_len);

                        bson_append_string(&bs, "type", "peer");
                        
                        bson_append_start_object(&bs, "peer");
                        bson_append_binary(&bs, "luid", (uint8_t*) identity->uid, WISH_ID_LEN);
                        bson_append_binary(&bs, "ruid", (uint8_t*) identity->uid, WISH_ID_LEN);
                        bson_append_binary(&bs, "rsid", (uint8_t*) service_registry[j].wsid, WISH_WSID_LEN);
                        bson_append_binary(&bs, "rhid", (uint8_t*) local_hostid, WISH_ID_LEN);
                        /* FIXME support more protocols than just one */
                        bson_append_string(&bs, "protocol", &(service_registry[j].protocols[0][0]));
                        
                        bson_append_string(&bs, "type", "N");   /* FIXME will be type:"D" someday when deleting identity? */
                        bson_append_bool(&bs, "online", online);
                        bson_append_finish_object(&bs);

                        bson_finish(&bs);
                        if (bs.err) {
                            WISHDEBUG(LOG_CRITICAL, "BSON error when creating peer message: %i %s len %i", bs.err, bs.errstr, bs.dataSize);
                        }
                        else {
                            //WISHDEBUG(LOG_CRITICAL, "Sending peer message to app %s:", service_registry[i].service_name);
                            //bson_visit("Sending peer message to app:", buffer);
                            send_core_to_app(core, service_registry[i].wsid, (uint8_t *) bson_data(&bs), bson_size(&bs));
                        }
                    }
                }
            }
        }
    }
}
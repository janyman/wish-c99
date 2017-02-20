#ifndef WISH_PORT_CONFIG_H
#define WISH_PORT_CONFIG_H

/** Port-specific config variables */

/** This specifies the size of the receive ring buffer */
#define WISH_PORT_RX_RB_SZ 1500

/** This specifies the maximum number of simultaneous Wish connections
 * */
#define WISH_PORT_CONTEXT_POOL_SZ   3

/** If this is defined, include support for the App TCP server */
//#define WITH_APP_TCP_SERVER

/** If this is defined, any friend request is accepted automatically if
 * id database contains just one entry */
#define WISH_ACCEPT_ANY_FRIEND_REQ_IF_NO_FRIENDS

/** If this is defined, all fried requests are automatically accepted! */
//#define WISH_ALLOW_ALL_FRIEND_REQS

/** This specifies the maximum size of the buffer where some RPC handlers build the reply (1400) */
#define WISH_PORT_RPC_BUFFER_SZ ( 1400 )

/** This defines the maximum number of entries in the Wish local discovery table (4).
 * You should make sure that in the worst case any message will fit into WISH_PORT_RPC_BUFFFER_SZ  */
#define WISH_LOCAL_DISCOVERY_MAX ( 4 ) /* wld.list: 64 local discoveries should fit in 16k RPC buffer size */

/** This defines the maximum number of uids in database (max number of identities + contacts) (4) 
     You should make sure that in the worst case any message will fit into WISH_PORT_RPC_BUFFFER_SZ */
#define WISH_PORT_MAX_UIDS ( 4 ) /* identity.list: 128 uid entries should fit into 16k RPC buffer */

/** When the Wish core is embedded into the main application WITH_APP_INTERNAL needs to be set */
#define WITH_APP_INTERNAL

#endif
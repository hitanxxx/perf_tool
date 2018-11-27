#ifndef _L_UPSTREAM_H_INCLUDED_
#define _L_UPSTREAM_H_INCLUDED_

#define UP_TIMEOUT 5

typedef struct upstream_send_t {

	string_t  		host;
	string_t  		uri;
	meta_t*			body;
} upstream_send_t;

typedef struct upstream_t upstream_t;
typedef status (* upstream_handler )( upstream_t * );
typedef struct upstream_t {
	connection_t * 				upstream;
	connection_t * 				downstream;

	struct sockaddr_in			up_addr;
	upstream_send_t				upstream_send;

	meta_t * 					request_meta;
	upstream_handler			handler;
	net_transport_t * 			transport;
} upstream_t;

status upstream_alloc( upstream_t ** up );
status upstream_free( upstream_t * up );
status upstream_start( void * data, json_t * json );

#endif

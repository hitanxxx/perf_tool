#ifndef _L_PERFORM_H_INCLUDED_
#define _L_PERFORM_H_INCLUDED_

#define 		PERFORM_MAX_PIPE					10
#define 		PERFORM_TIME_OUT					3
#define 		PERFORM_KEEP_ALIVE_TIME_OUT			PERFORM_TIME_OUT
#define 		PERFORM_SETTING_LENGTH				32768
#define 		RES_CODE(p) ( p->response_head->http_status_code )

typedef struct perform_setting_t {
	uint32 			concurrent;
	uint32 			keepalive;
	uint32			running_time_sec;
	timer_msg_t		running_timer;
	mem_list_t *	list_pipeline;
} perform_setting_t;

typedef struct perform_count_t {
	l_atomic_t	*	perform_success;
	l_atomic_t	*	perform_failed;
	l_atomic_t	*	perform_recvs;
	l_atomic_t	*	perform_sends;

	l_atomic_t	*	perform_200;
	l_atomic_t	*	perform_1xx;
	l_atomic_t	*	perform_2xx;
	l_atomic_t	*	perform_3xx;
	l_atomic_t	*	perform_4xx;
	l_atomic_t	*	perform_5xx;
} perform_count_t;

typedef struct perform_pipeline_t {
	l_mem_page_t *		page;
	struct sockaddr_in 	addr;
	meta_t*		request_meta;

	uint32		index;
	uint32		https;
	uint32		port;
	string_t 	ip;
	string_t	method;
	string_t	uri;
	string_t 	host;
	string_t	body;
} perform_pipeline_t;

typedef struct perform_t perform_t;
typedef status (*perform_handler)( perform_t * );
struct perform_t {
	queue_t					queue;
	connection_t* 			c;
	perform_handler 		handler;

	http_response_head_t *		response_head;
	http_entitybody_t *			response_body;
	meta_t						send_chain;

	uint32					send_n;
	uint32					recv_n;

	uint32					pipeline_index;
	perform_pipeline_t*		pipeline;
};


status performance_process_running( void );
status performance_process_start ( void );
status performance_process_stop ( void );

status performance_count_output( json_t ** data );

status perform_process_init( void );
status perform_process_end( void );

status perform_end( void );
status perform_init( void );
#endif

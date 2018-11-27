#include "lk.h"

static queue_t 				in_use;
static queue_t 				usable;
static perform_t*			pool = NULL;
static perform_setting_t 	perf_settings;
static l_atomic_t single_process_count_arr[PERFORM_MAX_PIPE] = {0};
static perform_count_t		perf_count;

static uint32	shm_length = 0;
static char * 	shm_ptr = NULL;

static status performance_setting_end( void );
static status perf_prepare( perform_t ** p );
static status perf_go( perform_t * p );
static status perf_over( perform_t * p, status rc );
static status performance_count_change( l_atomic_t * count, uint32 value );

// perf_alloc ------------------------------------------
static status perf_alloc( perform_t ** t )
{
	perform_t * new;
	queue_t * q;

	if( 1 == queue_empty( &usable ) ) {
		err_log(  "%s --- usable empty", __func__ );
		return ERROR;
	}

	q = queue_head( &usable );
	queue_remove( q );
	queue_insert_tail( &in_use, q );
	new = l_get_struct( q, perform_t, queue );
	*t = new;
	return OK;
}
// perf_free ----------------------------------------------------
static status perf_free( perform_t * p )
{
	queue_remove( &p->queue );
	queue_insert_tail( &usable, &p->queue );

	p->c = NULL;
	p->handler = NULL;

	p->response_head = NULL;
	p->response_body = NULL;
	p->send_chain.pos = p->send_chain.last = p->send_chain.start;
	p->send_chain.next = NULL;

	p->send_n  = 0;
	p->recv_n = 0;

	p->pipeline_index = 0;
	p->pipeline = NULL;
	return OK;
}
// perf_free_connection --------------
static status perf_free_connection( event_t * ev )
{
	connection_t * c;

	c = ev->data;

	net_free( c );
	c = NULL;
	return OK;
}
// perf_close_connection --------------
static status perf_close_connection( connection_t * c )
{
	int32 rc;

	if( c->ssl_flag && c->ssl ) {
		rc = ssl_shutdown( c->ssl );
		if( rc == AGAIN ) {
			c->ssl->handler = perf_free_connection;
			return AGAIN;
		}
	}
	perf_free_connection( c->write );
	return OK;
}
// perf_close_perform -----------------
static status perf_close_perform( perform_t * p )
{
	if( p->response_head ) {
		http_response_head_free( p->response_head );
	}
	if( p->response_body ) {
		http_entitybody_free( p->response_body );
	}
	perf_free( p );
	p = NULL;
	return OK;
}
// perf_timeout_connection ---------
static void perf_timeout_connection( void * data )
{
	connection_t * c;

	c = data;
	perf_close_connection( c );
}
// perf_time_out -----------
static void perf_time_out( void * data )
{
	perform_t * p;

	p = data;
	debug_log(  "%s --- ", __func__ );
	perf_over( p, ERROR );
}
// perf_test_reading ---------------------
static status perf_test_reading( event_t * ev )
{
	char buf[1];
	connection_t * c;
	perform_t * p;
	socklen_t len;
	int32 err;
	ssize_t n;

	c = ev->data;
	p = c->data;
	len = sizeof(errno);
	if( getsockopt( c->fd, SOL_SOCKET, SO_ERROR, (void*)&err, &len ) == -1 ) {
		err = errno;
	}
	goto closed;

	n = recv( c->fd, buf, 1, MSG_PEEK );
	if( n == -1 ) {
		goto closed;
	} else if ( n == 0 ) {
		goto closed;
	}
	return OK;
closed:
	// peer closed, stop it,
	err_log(  "%s --- peer closed", __func__ );
	perf_over( p, ERROR );
	return  OK;
}
// perf_over ----------
static status perf_over( perform_t * p, status rc )
{
	connection_t * c;
	uint32 old_index;

	c = p->c;
	old_index = p->pipeline_index;
	if( rc == DONE ) {
		// process need be stop
		perf_close_connection( p->c );
		perf_close_perform( p );
	} else if ( rc == OK ) {
		// success, change flag, continue
		// if we got a complate response, but it's not 200 response status.
		// we still think that it's success
		debug_log(  "%s --- perform over, OK", __func__ );
		performance_count_change( &perf_count.perform_success[p->pipeline_index], 1 );
		if( RES_CODE(p) == 200 ) {
			performance_count_change( &perf_count.perform_200[p->pipeline_index], 1 );
		} else if ( 100 <= RES_CODE(p) && RES_CODE(p) < 200 ) {
			performance_count_change( &perf_count.perform_1xx[p->pipeline_index], 1 );
		} else if ( 200 <  RES_CODE(p) && RES_CODE(p) < 300 ) {
			performance_count_change( &perf_count.perform_2xx[p->pipeline_index], 1 );
		} else if ( 300 <= RES_CODE(p) && RES_CODE(p) < 400 ) {
			performance_count_change( &perf_count.perform_3xx[p->pipeline_index], 1 );
		} else if ( 400 <= RES_CODE(p) && RES_CODE(p) < 500 ) {
			performance_count_change( &perf_count.perform_4xx[p->pipeline_index], 1 );
		} else if ( 500 <= RES_CODE(p) && RES_CODE(p) < 600 ) {
			performance_count_change( &perf_count.perform_5xx[p->pipeline_index], 1 );
		}
		// if keep alive on, don't l_safe_free the connection, set a keepalive timer
		if( p->response_head->keepalive && perf_settings.keepalive ) {
			c->read->handler = perf_test_reading;
			c->write->handler = NULL;

			// waiting time out
			c->read->timer.data = (void*)c;
			c->read->timer.handler = perf_timeout_connection;
			timer_add( &c->read->timer, PERFORM_KEEP_ALIVE_TIME_OUT );
		} else {
			perf_close_connection( c );
		}
		perf_close_perform( p );
		perf_prepare( &p );
		p->pipeline_index = old_index;
		p->pipeline = mem_list_get( perf_settings.list_pipeline,
			p->pipeline_index + 1 );
		return perf_go( p );
	} else if ( rc == ERROR ) {
		// got a error, change flag, continue
		performance_count_change( &perf_count.perform_failed[p->pipeline_index], 1 );
		perf_close_connection( c );
		perf_close_perform( p );

		perf_prepare( &p );
		p->pipeline_index = old_index;
		p->pipeline = mem_list_get( perf_settings.list_pipeline,
			p->pipeline_index + 1 );
		return perf_go( p );
	}
	return OK;
}
// perf_recv_body ----------
static status perf_recv_body( event_t * ev )
{
	connection_t * c;
	perform_t * p;
	int32 rc;

	c = ev->data;
	p = c->data;
	rc = p->response_body->handler( p->response_body );
	if( rc == ERROR ) {
		err_log(  "%s --- body handler", __func__ );
		perf_over( p, ERROR );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &c->read->timer );
		debug_log(  "%s --- success", __func__ );
		performance_count_change( &perf_count.perform_recvs[p->pipeline_index],
			p->response_body->all_length );
		return perf_over( p, OK );
	}
	c->read->timer.data = (void*)p;
	c->read->timer.handler = perf_time_out;
	timer_add( &c->read->timer, PERFORM_TIME_OUT );
	return rc;
}
// perf_recv_header ----------
static status perf_recv_header ( event_t * ev )
{
	int32 rc;
	perform_t * p;
	connection_t * c;

	c = ev->data;
	p = c->data;
	rc = p->response_head->handler( p->response_head );
	if( rc == ERROR ) {
		err_log(  "%s --- response handler", __func__ );
		perf_over( p, ERROR );
		return ERROR;
	} else if( rc == DONE ) {
		timer_del( &c->read->timer );
		debug_log(  "%s --- success", __func__ );
		performance_count_change( &perf_count.perform_recvs[p->pipeline_index],
		 	meta_len( p->response_head->head.pos, p->response_head->head.last) );

		// have't any response body
		if(	p->response_head->body_type == HTTP_ENTITYBODY_NULL ) {
			return perf_over( p, OK );
		}
		if( OK != http_entitybody_create( c, &p->response_body ) ) {
			err_log(  "%s --- entity body create", __func__ );
			perf_over( p, ERROR );
			return ERROR;
		}
		p->response_body->cache = 0;
		p->response_body->body_type = p->response_head->body_type;
		p->response_body->content_length = p->response_head->content_length;

		c->read->handler = perf_recv_body;
		return c->read->handler( c->read );
	}
	c->read->timer.data = (void*)p;
	c->read->timer.handler = perf_time_out;
	timer_add( &c->read->timer, PERFORM_TIME_OUT );
	return rc;
}
// perf_send ------------
static status perf_send( event_t * ev )
{
	connection_t * c;
	int32 rc;
	perform_t * p;

	c = ev->data;
	p = c->data;
	rc = c->send_chain( c, &p->send_chain );
	if( rc == ERROR ) {
		err_log(  "%s --- send chain", __func__ );
		perf_over( p, ERROR );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &c->write->timer );
		debug_log(  "%s --- success", __func__ );
		performance_count_change( &perf_count.perform_sends[p->pipeline_index],
	 	p->send_n );

		if( OK != http_response_head_create( c, &p->response_head ) ) {
			err_log(  "%s --- http_response_head_create", __func__ );
			perf_over( p, ERROR );
			return ERROR;
		}
		event_opt( c->read, EVENT_READ );
		c->read->handler = perf_recv_header;
		c->write->handler = NULL;

		return c->read->handler( c->read );
		// c->read->timer.data = (void*)p;
		// c->read->timer.handler = perf_time_out;
		// timer_add( &c->read->timer, PERFORM_TIME_OUT );
		// return AGAIN;
	}
	c->write->timer.data = (void*)p;
	c->write->timer.handler = perf_time_out;
	timer_add( &c->write->timer, PERFORM_TIME_OUT );
	return rc;
}
// perf_send_prepare_request ------------
static status perf_send_prepare_request( perform_pipeline_t * pipeline )
{
	uint32 request_length = 0;
	char * p;

	// method uri HTTP/1.1\r\n
	request_length += pipeline->method.len;
	request_length += pipeline->uri.len;
	request_length += l_strlen("  HTTP/1.1\r\n");
	// Host: host\r\n
	request_length += l_strlen("Host: \r\n");
	request_length += pipeline->host.len;
	// User-Agent: lk-perf-v1\r\n
	request_length += l_strlen("User-Agent: lk-perf-v1\r\n");
	// Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n
	request_length += l_strlen("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n");
	// Accept-Encoding: deflate\r\n
	request_length += l_strlen("Accept-Encoding: deflate\r\n");
	// Accept-Language: zh-CN,zh;q=0.9\r\n
	request_length += l_strlen("Accept-Language: zh-CN,zh;q=0.9\r\n");
	// Connection: Close/Keep-Alive\r\n
	if( perf_settings.keepalive ) {
		request_length += l_strlen("Connection: Keep-Alive\r\n");
	} else {
		request_length += l_strlen("Connection: Close\r\n");
	}
	// \r\n
	request_length += l_strlen("\r\n");
	// body part
	if( pipeline->method.len == l_strlen("post") ) {
		if( strncmp( pipeline->method.data, "post", l_strlen("post") ) == 0 ||
			strncmp( pipeline->method.data, "POST", l_strlen("POST") ) == 0 ) {
			if( pipeline->body.len > 0 ) {
				request_length += pipeline->body.len;
			}
		}
	} else if ( pipeline->method.len == l_strlen("put") ) {
		if( strncmp( pipeline->method.data, "put", l_strlen("put") ) == 0 ||
			strncmp( pipeline->method.data, "PUT", l_strlen("PUT") ) == 0  ) {
			if( pipeline->body.len > 0 ) {
				request_length += pipeline->body.len;
			}
		}
	}
	// build request string
	pipeline->request_meta = l_mem_alloc( pipeline->page, request_length +
 	(uint32)sizeof(meta_t) );
	if( !pipeline->request_meta ) {
		err_log("%s --- mem alloc pipeline's request meta", __func__ );
		return ERROR;
	}
	memset( pipeline->request_meta, 0, request_length + sizeof(meta_t) );
	pipeline->request_meta->next = NULL;
	pipeline->request_meta->data = (char*)pipeline->request_meta + (uint32)sizeof(meta_t);
	pipeline->request_meta->start = pipeline->request_meta->pos =
	pipeline->request_meta->last = pipeline->request_meta->data;
	pipeline->request_meta->end = pipeline->request_meta->start + request_length;

	p = pipeline->request_meta->data;
	// method uri HTTP/1.1\r\n
	memcpy( p, pipeline->method.data, pipeline->method.len );
	p += pipeline->method.len;
	memcpy( p, " ", l_strlen(" ") );
	p += l_strlen(" ");
	memcpy( p, pipeline->uri.data, pipeline->uri.len );
	p += pipeline->uri.len;
	memcpy( p, " HTTP/1.1\r\n", l_strlen(" HTTP/1.1\r\n") );
	p += l_strlen(" HTTP/1.1\r\n");
	// Host: host\r\n
	memcpy( p, "Host: ", l_strlen("Host: ") );
	p += l_strlen("Host: ");
	memcpy( p, pipeline->host.data, pipeline->host.len );
	p += pipeline->host.len;
	memcpy( p, "\r\n", l_strlen("\r\n") );
	p += l_strlen("\r\n");
	// User-Agent: lk-perf-v1\r\n
	memcpy( p, "User-Agent: lk-perf-v1\r\n",
	l_strlen("User-Agent: lk-perf-v1\r\n") );
	p += l_strlen("User-Agent: lk-perf-v1\r\n");
	// Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n
	memcpy( p, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n",
 	l_strlen("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n"));
	p += l_strlen("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n");
	// Accept-Encoding: deflate\r\n
	memcpy( p, "Accept-Encoding: deflate\r\n", l_strlen("Accept-Encoding: deflate\r\n") );
	p += l_strlen("Accept-Encoding: deflate\r\n");
	// Accept-Language: zh-CN,zh;q=0.9\r\n
	memcpy( p, "Accept-Language: zh-CN,zh;q=0.9\r\n", l_strlen("Accept-Language: zh-CN,zh;q=0.9\r\n") );
	p += l_strlen("Accept-Language: zh-CN,zh;q=0.9\r\n");
	// Connection: Close/Keep-Alive\r\n
	if( perf_settings.keepalive ) {
		memcpy( p, "Connection: Keep-Alive\r\n",
		l_strlen("Connection: Keep-Alive\r\n") );
		p += l_strlen("Connection: Keep-Alive\r\n");
	} else {
		memcpy( p, "Connection: Close\r\n",
		l_strlen("Connection: Close\r\n") );
		p += l_strlen("Connection: Close\r\n");
	}
	// ---
	memcpy( p, "\r\n", l_strlen("\r\n") );
	p += l_strlen("\r\n");

	if( pipeline->method.len == l_strlen("post") ) {
		if( strncmp( pipeline->method.data, "post", l_strlen("post") ) == 0 ||
	strncmp( pipeline->method.data, "POST", l_strlen("POST") ) == 0 ) {
			if( pipeline->body.len > 0 ) {
				memcpy( p, pipeline->body.data, pipeline->body.len );
				p += pipeline->body.len;
			}
		}
	} else if ( pipeline->method.len == l_strlen("put") ) {
		if( strncmp( pipeline->method.data, "put", l_strlen("put") ) == 0 ||
	strncmp( pipeline->method.data, "PUT", l_strlen("PUT") ) == 0 ) {
			if( pipeline->body.len > 0 ) {
				memcpy( p, pipeline->body.data, pipeline->body.len );
				p += pipeline->body.len;
			}
		}
	}
	pipeline->request_meta->last = p;
	return OK;
}
// perf_send_prepare ---------------
static status perf_send_prepare( event_t * ev )
{
	connection_t * c;
	perform_t * p;

	c = ev->data;
	p = c->data;

	if( !p->pipeline->request_meta ) {
		if( OK != perf_send_prepare_request( p->pipeline ) ) {
			err_log(  "%s --- create pipeline request data", __func__ );
			perf_over( p, ERROR );
			return ERROR;
		}
	}
	// each request meta chain only have one meta
	p->send_chain.pos = p->pipeline->request_meta->pos;
	p->send_chain.last = p->pipeline->request_meta->last;
	p->send_n = meta_len( p->send_chain.pos, p->send_chain.last );

	c->write->handler = perf_send;
	return c->write->handler( c->write );
}
// perf_ssl_handshake_handler ------------
static status perf_ssl_handshake_handler( event_t * ev )
{
	connection_t * c;
	perform_t * p;

	c = ev->data;
	p = c->data;

	if( !c->ssl->handshaked ) {
		err_log( "%s --- handshake error", __func__ );
		perf_over( p, ERROR );
		return ERROR;
	}
	timer_del( &c->write->timer );
	c->recv = ssl_read;
	c->send = ssl_write;
	c->recv_chain = NULL;
	c->send_chain = ssl_write_chain;

	c->write->handler = perf_send_prepare;
	return c->write->handler( c->write );
}
// perf_connect_test ------
static status perf_connect_test( connection_t * c )
{
	int        err;
    socklen_t  len = sizeof(err);

	if (getsockopt( c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) == -1) {
		err = errno;
	}
	if (err) {
		return ERROR;
	}
	return OK;
}
// perf_connect_check -----------
static status perf_connect_check ( event_t * ev )
{
	connection_t * c;
	perform_t * p;
	int32 rc;

	c = ev->data;
	p = c->data;
	if( c->write->timer.f_timeset ) {
		if( OK != perf_connect_test( c ) ) {
			err_log(  "%s --- connect test", __func__ );
			perf_over( p, ERROR );
			return ERROR;
		}
		timer_del( &c->write->timer );
	}
	debug_log(  "%s --- connect success", __func__ );

	if( p->pipeline->https ) {
		c->ssl_flag = 1;
		if( OK != ssl_create_connection( c, L_SSL_CLIENT ) ) {
			err_log(  "%s --- ssl connection create error", __func__ );
			perf_over( p, ERROR );
			return ERROR;
		}
		rc = ssl_handshake( c->ssl );
		if( rc == ERROR ) {
			err_log(  "%s --- handshake error", __func__ );
			perf_over( p, ERROR );
			return ERROR;
		} else if ( rc == AGAIN ) {
			c->write->timer.data = (void*)p;
			c->write->timer.handler = perf_time_out;
			timer_add( &c->write->timer, PERFORM_TIME_OUT );

			c->ssl->handler = perf_ssl_handshake_handler;
			return AGAIN;
		}
		return perf_ssl_handshake_handler( ev );
	}
	c->write->handler = perf_send_prepare;
	return c->write->handler( c->write );
}
// perf_connect --------------
static status perf_connect( event_t * ev )
{
	int32 rc;
	connection_t * c;
	perform_t * p;

	c = ev->data;
	p = c->data;
	memcpy( &c->addr, &p->pipeline->addr, sizeof(struct sockaddr_in) );
	rc = event_connect( c->write );
	if( rc == ERROR ) {
		err_log(  "%s --- connect error", __func__ );
		perf_over( p, ERROR );
		return ERROR;
	}
	event_opt( c->write, EVENT_WRITE );
	c->write->handler = perf_connect_check;

	if( rc == AGAIN ) {
		debug_log(  "%s --- connect again", __func__ );
		c->write->timer.data = (void*)p;
		c->write->timer.handler = perf_time_out;
		timer_add( &c->write->timer, PERFORM_TIME_OUT );
		return AGAIN;
	}
	return c->write->handler( c->write );
}
// perf_go ---------
static status perf_go( perform_t * p )
{
	return p->c->write->handler( p->c->write );
}
// perf_prepare --------
static status perf_prepare( perform_t ** p )
{
	perform_t * new;
	connection_t * c;

	if( OK != perf_alloc( &new ) ) {
		err_log(  "%s --- alloc perf object", __func__ );
		return ERROR;
	}
	if( OK != net_alloc( &c ) ) {
		err_log(  "%s --- net_alloc error", __func__ );
		perf_close_perform( new );
		return ERROR;
	}
	c->send = sends;
	c->recv = recvs;
	c->send_chain = send_chains;
	c->recv_chain = NULL;
	if( !c->meta ) {
		// peer response head length limit value 4096
		if( OK != meta_alloc( &c->meta, 4096 ) ) {
			err_log( "%s --- c meta alloc", __func__ );
			perf_close_perform( new );
			return ERROR;
		}
	}
	new->c = c;
	c->data = (void*)new;

	c->read->handler = NULL;
	c->write->handler = perf_connect;
	*p = new;
	return OK;
}
// performance_count_change --------------
static status performance_count_change( l_atomic_t * count, uint32 value )
{
	/*
		count always increment, clean count if value is 0
	*/
	if( value == 0 ) {
		if( conf.worker_process <= 1 ) {
			*count = 0;
		} else {
			__sync_fetch_and_and( count, 0 );
		}
	} else {
		if( conf.worker_process <= 1 ) {
			*count += value;
		} else {
			__sync_fetch_and_add( count, value );
		}
	}
	return OK;
}
// performance_count_output --------------
status performance_count_output( json_t ** data )
{
	perform_pipeline_t * pipeline;
	uint32 i=0;
	json_t *json, *root, *ob_name, *ob_arr, *length_name, *length;
	json_t *obj, *v, *vv;
	string_t arr_name = string("result");
	string_t index_str = string("index");
	string_t success_str = string("success");
	string_t failed_str = string("failed");
	string_t recv_str = string("recv_byte");
	string_t send_str = string("send_byte");
	string_t length_str = string("length");
	string_t str_200 = string("200");
	string_t str_1xx = string("Informational");
	string_t str_2xx = string("Successful");
	string_t str_3xx = string("Redirection");
	string_t str_4xx = string("Client Error");
	string_t str_5xx = string("Server Error");

	if( ERROR == json_create( &json ) ) {
		err_log(  "%s --- json create", __func__ );
		return ERROR;
	}
	root = mem_list_push( json->list );
	root->type = JSON_OBJ;

	mem_list_create( &root->list, sizeof(json_t) );
	length_name = mem_list_push( root->list );
	length_name->type = JSON_STR;
	length_name->name.data = length_str.data;
	length_name->name.len = length_str.len;
	mem_list_create( &length_name->list, sizeof(json_t) );
	length = mem_list_push( length_name->list );
	length->type = JSON_NUM;
	length->num = perf_settings.list_pipeline ?
	perf_settings.list_pipeline->elem_num : 0;

	ob_name = mem_list_push( root->list );
	ob_name->type = JSON_STR;
	ob_name->name.data = arr_name.data;
	ob_name->name.len = arr_name.len;
	mem_list_create( &ob_name->list, sizeof(json_t) );
	ob_arr = mem_list_push( ob_name->list );
	ob_arr->type = JSON_ARR;

	mem_list_create( &ob_arr->list, sizeof(json_t) );
	if( perf_settings.list_pipeline ) {
		for( i = 1; i <= perf_settings.list_pipeline->elem_num; i ++ ) {
			pipeline = mem_list_get( perf_settings.list_pipeline, i );
			obj = mem_list_push( ob_arr->list );
			obj->type = JSON_OBJ;
			mem_list_create( &obj->list, sizeof(json_t) );

			// index
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = index_str.data;
			v->name.len = index_str.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = pipeline->index;
			// success
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = success_str.data;
			v->name.len = success_str.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_success[i-1];
			// failed
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = failed_str.data;
			v->name.len = failed_str.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_failed[i-1];
			// send
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = send_str.data;
			v->name.len = send_str.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_sends[i-1];
			// recv
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = recv_str.data;
			v->name.len = recv_str.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_recvs[i-1];
			// 200
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = str_200.data;
			v->name.len = str_200.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_200[i-1];
			// 1xx
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = str_1xx.data;
			v->name.len = str_1xx.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_1xx[i-1];
			// 2xx
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = str_2xx.data;
			v->name.len = str_2xx.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_2xx[i-1];
			// 3xx
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = str_3xx.data;
			v->name.len = str_3xx.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_3xx[i-1];
			// 4xx
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = str_4xx.data;
			v->name.len = str_4xx.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_4xx[i-1];
			// 5xx
			v = mem_list_push( obj->list );
			v->type = JSON_STR;
			v->name.data = str_5xx.data;
			v->name.len = str_5xx.len;
			mem_list_create( &v->list, sizeof(json_t) );
			vv = mem_list_push( v->list );
			vv->type = JSON_NUM;
			vv->num = perf_count.perform_5xx[i-1];
		}
	}
	*data = json;
	return OK;
}
// performance_count_init -----------
static status performance_count_init( void )
{
	uint32 i;
	// clear share memory count data
	for( i = 0; i < PERFORM_MAX_PIPE; i ++ ) {
		performance_count_change( &perf_count.perform_success[i], 0 );
		performance_count_change( &perf_count.perform_failed[i], 0 );
		performance_count_change( &perf_count.perform_sends[i], 0 );
		performance_count_change( &perf_count.perform_recvs[i], 0 );

		performance_count_change( &perf_count.perform_200[i], 0 );
		performance_count_change( &perf_count.perform_1xx[i], 0 );
		performance_count_change( &perf_count.perform_2xx[i], 0 );
		performance_count_change( &perf_count.perform_3xx[i], 0 );
		performance_count_change( &perf_count.perform_4xx[i], 0 );
		performance_count_change( &perf_count.perform_5xx[i], 0 );
	}
	performance_setting_end( );
	return OK;
}
// performance_setting_end -------------
static status performance_setting_end( void )
{
	perform_pipeline_t * pipeline;
	uint32 i;

	if( perf_settings.list_pipeline ) {
		for( i = 0; i < perf_settings.list_pipeline->elem_num; i ++ ) {
			pipeline = mem_list_get( perf_settings.list_pipeline, i+1 );
			l_mem_page_free( pipeline->page );
		}
		mem_list_free( perf_settings.list_pipeline );
		perf_settings.list_pipeline = NULL;
	}
	return OK;
}
// performance_setting_repeat ----------
static status performance_setting_repeat( uint32 https,
string_t *ip,
uint32 port,
string_t *method,
string_t *uri,
string_t *host )
{
	perform_pipeline_t * pipeline;
	uint32 i;

	for( i = 1; i <= perf_settings.list_pipeline->elem_num; i ++ ) {
		pipeline = mem_list_get( perf_settings.list_pipeline, i );
		if( pipeline->https == https &&
			pipeline->port == port &&
		 	pipeline->ip.len == ip->len &&
			pipeline->method.len == method->len &&
			pipeline->uri.len == uri->len &&
			pipeline->host.len == host->len &&
			strncmp( pipeline->ip.data, ip->data, ip->len ) == 0 &&
			strncmp( pipeline->method.data, method->data, method->len ) == 0 &&
			strncmp( pipeline->uri.data, uri->data, uri->len ) == 0 &&
			strncmp( pipeline->host.data, host->data, host->len ) == 0
		) {
			return OK;
		}
	}
	return ERROR;
}
// performance_setting_init --------------
static status performance_setting_init( json_t * json )
{
	json_t *t, *x, *v;
	uint32 i;
	char str[1024];
	perform_pipeline_t * pipeline;
	string_t ip, host, method, uri;
	uint32 port, https;

	// perform setting data has been checked when saved
	json_get_child( json, 1, &t );
	if( t->type != JSON_OBJ ) {
		err_log(  "%s --- root not obj", __func__ );
		return ERROR;
	}
	json_get_obj_num( t, "time", l_strlen("time"), &x );
	perf_settings.running_time_sec = (uint32)x->num;

	json_get_obj_num( t, "concurrent", l_strlen("concurrent"), &x );
	perf_settings.concurrent = (uint32)x->num;

	json_get_obj_bool( t, "keepalive", l_strlen("keepalive"), &x );
	perf_settings.keepalive = (x->type == JSON_TRUE) ? 1 : 0;

	debug_log(  "%s --- perf_setting_time_n	[%d]",
	__func__, perf_settings.running_time_sec );
	debug_log(  "%s --- perf_setting_concurrent_n	[%d]",
	__func__, perf_settings.concurrent );
	debug_log(  "%s --- perf_setting_keepalive_bool	[%d]",
	__func__, perf_settings.keepalive );

	json_get_obj_arr( t, "pipeline", l_strlen("pipeline"), &x );
	mem_list_create( &perf_settings.list_pipeline, sizeof(perform_pipeline_t) );
	for( i = 1; i <= x->list->elem_num; i ++ ) {
		pipeline = mem_list_push( perf_settings.list_pipeline );
		if( OK != l_mem_page_create( &pipeline->page, 4096 ) ) {
			err_log("%s --- mem page create", __func__ );
			return ERROR;
		}
		memset( &pipeline->addr, 0, sizeof(pipeline->addr) );
		json_get_child( x, i, &t );

		json_get_obj_num( t, "index", l_strlen("index"), &v );
		pipeline->index = (uint32)v->num;
		json_get_obj_bool( t, "https", l_strlen("https"), &v );
		https = (v->type == JSON_TRUE ) ? 1 : 0;
		json_get_obj_num( t, "port", l_strlen("port"), &v );
		port = (uint32)v->num;
		json_get_obj_str( t, "ip", l_strlen("ip"), &v );
		ip.data = v->name.data;
		ip.len = v->name.len;
		json_get_obj_str( t, "host", l_strlen("host"), &v );
		host.data = v->name.data;
		host.len = v->name.len;
		json_get_obj_str( t, "method", l_strlen("method"), &v );
		method.data = v->name.data;
		method.len = v->name.len;
		json_get_obj_str( t, "uri", l_strlen("uri"), &v );
		uri.data = v->name.data;
		uri.len = v->name.len;

		if( OK == performance_setting_repeat( https,
			&ip,
			port,
		 	&method,
			&uri,
			&host ) ) {
			err_log(  "%s --- perform setting data reapeat", __func__ );
			return ERROR;
		}
		// give value
		pipeline->https = https;
		pipeline->port = port;
		pipeline->ip.len = ip.len;
		pipeline->ip.data = l_mem_alloc( pipeline->page, pipeline->ip.len );
		if( !pipeline->ip.data ) {
			err_log("%s --- mem alloc ip data", __func__ );
			return ERROR;
		}
		memcpy( pipeline->ip.data, ip.data, ip.len );
		// ---
		pipeline->addr.sin_family = AF_INET;
		memset( str, 0, sizeof(str) );
		memcpy( str, pipeline->ip.data, pipeline->ip.len );
		pipeline->addr.sin_addr.s_addr = inet_addr( str );
		pipeline->addr.sin_port = htons( pipeline->port );
		// ---
		pipeline->host.len = host.len;
		pipeline->host.data = l_mem_alloc( pipeline->page, pipeline->host.len );
		if( !pipeline->host.data ) {
			err_log(  "%s --- mem alloc host data", __func__ );
			return ERROR;
		}
		memcpy( pipeline->host.data, host.data, host.len );
		// ---
		pipeline->method.len = method.len;
		pipeline->method.data = l_mem_alloc( pipeline->page, pipeline->method.len );
		if( !pipeline->method.data ) {
			err_log(  "%s --- mem alloc method data", __func__ );
			return ERROR;
		}
		memcpy( pipeline->method.data, method.data, method.len );
		// ---
		pipeline->uri.len = uri.len;
		pipeline->uri.data = l_mem_alloc( pipeline->page, pipeline->uri.len );
		if( !pipeline->uri.data ) {
			err_log(  "%s --- mem alloc uri data", __func__ );
			return ERROR;
		}
		memcpy( pipeline->uri.data, uri.data, uri.len );
		// ---
		debug_log(  "%s -------------------", __func__ );
		debug_log( "%s --- https [%d]", __func__,
	 	pipeline->https );
		debug_log(  "%s --- ip	[%.*s]", __func__,
		pipeline->ip.len, pipeline->ip.data );
		debug_log(  "%s --- port	[%d]", 	 __func__,
		pipeline->port );
		debug_log(  "%s --- host	[%.*s]", __func__,
		pipeline->host.len, pipeline->host.data );
		debug_log(  "%s --- uri	[%.*s]", __func__,
		pipeline->uri.len, pipeline->uri.data );
		debug_log(  "%s --- method	[%.*s]", __func__,
		pipeline->method.len, pipeline->method.data );
		if ( OK == json_get_obj_str( t, "body", l_strlen("body"), &v ) ) {
			pipeline->body.len = v->name.len;
			pipeline->body.data = l_mem_alloc( pipeline->page, pipeline->body.len );
			if( !pipeline->body.data ) {
				err_log(  "%s --- mem alloc body data", __func__ );
				return ERROR;
			}
			memcpy( pipeline->body.data, v->name.data, v->name.len );
			debug_log(  "%s --- body	[%.*s]", __func__,
			pipeline->body.len, pipeline->body.data );
		}
	}
	return OK;
}
// performance_process_time_out ------------
static void performance_process_time_out( void * data )
{
	l_unused( data );
	performance_process_stop(  );
}
// performance_process_stop ----------------
status performance_process_stop ( void )
{
	queue_t *q, *next;
	perform_t * perform;

	if( queue_empty( &in_use ) ) {
		return OK;
	}
	// clear perform running queue, stop all perform
	q = queue_head( &in_use );
	while( q != queue_tail( &in_use ) ) {
		next = queue_next( q );
		perform = l_get_struct( q, perform_t, queue );
		perf_over( perform, DONE );
		q = next;
	}
	timer_del( &perf_settings.running_timer );
	return OK;
}
// performance_process_running ----------
status performance_process_running( void )
{
	return perf_settings.running_timer.f_timeset;
}
// performance_process_prepare ------------------
static status performance_process_prepare ( void * data )
{
	json_t * json;
	uint32 i, k;
	perform_t * p;

	json = (json_t*)data;
	// in running
	if( performance_process_running() ) {
		err_log("%s --- perform test is running", __func__ );
		return ERROR;
	}
	// clear count data
	if( OK != performance_count_init( ) ) {
		err_log(  "%s --- clear pipeline", __func__ );
		return ERROR;
	}
	// get infomation
	if( OK != performance_setting_init( json ) ) {
		err_log(  "%s --- get json's infomation", __func__ );
		return ERROR;
	}
	// start
	perf_settings.running_timer.data = NULL;
	perf_settings.running_timer.handler = performance_process_time_out;
	timer_add( &perf_settings.running_timer, perf_settings.running_time_sec );
	/*
		each pipeline have concurrent num instance
	*/
	for( i = 0; i < perf_settings.list_pipeline->elem_num; i ++ ) {
		for( k = 0; k < perf_settings.concurrent; k ++ ) {
			perf_prepare( &p );
			p->pipeline_index = i;
			p->pipeline = mem_list_get( perf_settings.list_pipeline,
				p->pipeline_index + 1 );
			perf_go( p );
		}
	}
	return OK;
}
// performance_process_start -------------
status performance_process_start ( void )
{
	meta_t * t;
	json_t * json;

	if( OK != config_get( &t, L_PATH_PERFTEMP ) ) {
		err_log( "%s --- get perf data", __func__ );
		return ERROR;
	}
	if( OK != json_decode( &json, t->pos, t->last ) ) {
		err_log( "%s --- json decode", __func__ );
		meta_free( t );
		return ERROR;
	}
	performance_process_prepare( (void*)json );
	json_free( json );
	meta_free( t );
	return OK;
}
// perform_process_init -------------------------
status perform_process_init( void )
{
	int32 i;

	queue_init( &usable );
	queue_init( &in_use );
	pool = ( perform_t *) l_safe_malloc ( sizeof(perform_t) * MAXCON );
	if( !pool ) {
		err_log(  "%s --- perform pool alloc", __func__ );
		return ERROR;
	}
	memset( pool, 0, sizeof(perform_t)*MAXCON );
	for( i = 0; i < MAXCON; i ++ ) {
		queue_insert_tail( &usable, &pool[i].queue );
	}
	return OK;
}
// perform_process_end ---------------------------
status perform_process_end( void )
{
	if( pool ) {
		l_safe_free( pool );
	}
	return OK;
}
// perform_init ----------------
status perform_init( void )
{
	if( !conf.perf_switch ) {
		return OK;
	}
	if( conf.worker_process <= 1 ) {
		perf_count.perform_success = &single_process_count_arr[0];
		perf_count.perform_failed = &single_process_count_arr[1];
		perf_count.perform_sends = &single_process_count_arr[2];
		perf_count.perform_recvs = &single_process_count_arr[3];
		perf_count.perform_200 = &single_process_count_arr[4];
		perf_count.perform_1xx = &single_process_count_arr[5];
		perf_count.perform_2xx = &single_process_count_arr[6];
		perf_count.perform_3xx = &single_process_count_arr[7];
		perf_count.perform_4xx = &single_process_count_arr[8];
		perf_count.perform_5xx = &single_process_count_arr[9];
		return OK;
	}
	shm_length += (uint32)( sizeof(l_atomic_t) * PERFORM_MAX_PIPE * ( 4 + 6 ) );
	shm_ptr = (char*) mmap(NULL, shm_length, PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);
	if( shm_ptr == MAP_FAILED ) {
		err_log("%s --- mmap shm failed, [%d]", __func__, errno );
		return ERROR;
	}
	perf_count.perform_success = (l_atomic_t*)( shm_ptr );
	perf_count.perform_failed = (l_atomic_t*)
			( perf_count.perform_success + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_recvs = (l_atomic_t*)
			( perf_count.perform_failed + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_sends = (l_atomic_t*)
			( perf_count.perform_recvs + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_200 = (l_atomic_t*)
			( perf_count.perform_sends + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_1xx = (l_atomic_t*)
			( perf_count.perform_200 + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_2xx = (l_atomic_t*)
			( perf_count.perform_1xx + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_3xx = (l_atomic_t*)
			( perf_count.perform_2xx + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_4xx = (l_atomic_t*)
			( perf_count.perform_3xx + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	perf_count.perform_5xx = (l_atomic_t*)
			( perf_count.perform_4xx + (sizeof(l_atomic_t) * PERFORM_MAX_PIPE) );
	return OK;
}
// perform_end --------------
status perform_end( void )
{
	if( shm_ptr ) {
		munmap((void *) shm_ptr, shm_length );
	}
	shm_ptr = NULL;
	return OK;
}

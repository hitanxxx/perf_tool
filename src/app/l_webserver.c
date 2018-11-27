#include "lk.h"
// private data
static queue_t 	use;
static queue_t 	usable;
static webser_t * pool = NULL;

// 400
static string_t response_string_400 = string("HTTP/1.1 400 Bad Request\r\n"
"Connection: Close\r\n"
"Server: lk-v1.0\r\n"
"Content-type: text/html\r\n"
"Content-Length: 80\r\n\r\n"
"<html><title>400 bad Request</title><body>"
"<h1>400 bad Request</h1>"
"</body></html>");
// 403
static string_t response_string_403 = string("HTTP/1.1 403 Forbidden\r\n"
"Connection: Close\r\n"
"Server: lk-v1.0\r\n"
"Content-type: text/html\r\n"
"Content-Length: 76\r\n\r\n"
"<html><title>403 forbidden</title><body>"
"<h1>403 Forbidden</h1>"
"</body></html>");
// 404
static string_t response_string_404 = string("HTTP/1.1 404 Not Found\r\n"
"Connection: Close\r\n"
"Server: lk-v1.0\r\n"
"Content-type: text/html\r\n"
"Content-Length: 76\r\n\r\n"
"<html><title>404 not found</title><body>"
"<h1>404 Not Found</h1>"
"</body></html>");

static string_t default_mimetype =
					string("Content-type: application/octet-stream\r\n");
static mime_type_t mimetype_table[] =
{
	{string(".html"),			string("Content-type: text/html\r\n")},
	{string(".js"),
					string("Content-type: application/x-javascript\r\n")},
	{string(".json"),			string("Content-type: application/json\r\n")},
	{string(".png"),			string("Content-type: image/png\r\n")},
	{string(".jpg"),			string("Content-type: image/jpeg\r\n")},
	{string(".jpeg"),			string("Content-type: image/jpeg\r\n")},
	{string(".gif"),			string("Content-type: image/gif\r\n")},
	{string(".ico"),			string("Content-type: image/x-icon\r\n")},
	{string(".css"),			string("Content-type: text/css\r\n")},
	{string(".txt"),			string("Content-type: text/plain\r\n")},
	{string(".htm"),			string("Content-type: text/html\r\n")},
	{string(".mp3"),			string("Content-type: audio/mpeg\r\n")}
};

static status webser_process_request( event_t * ev );
static status webser_over_early( webser_t * webser, uint32 status_code);
status webser_over( webser_t * webser );

// webser_set_mimetype --------------------
status webser_set_mimetype( webser_t * webser, char * str, uint32 length )
{
	memcpy( webser->filepath, str, length );
	return OK;
}
// webser_get_mimetype -------------------------
static string_t * webser_get_mimetype( webser_t * webser )
{
	uint32 i;

	for( i =0; i < sizeof(mimetype_table)/sizeof(mime_type_t); i ++ ) {
		if( NULL != l_find_str( webser->filepath, l_strlen(webser->filepath),
		mimetype_table[i].type.data, mimetype_table[i].type.len ) ) {
			return &mimetype_table[i].header;
		}
	}
	err_log( "%s --- mimitype not found", __func__ );
	return NULL;
}
// webser_alloc ---------------
static status webser_alloc( webser_t ** webser )
{
	webser_t * new;
	queue_t * q;

	if( 1 == queue_empty( &usable ) ) {
		err_log( "%s --- don't have usbale", __func__ );
		return ERROR;
	}
	q = queue_head( &usable );
	queue_remove( q );
	queue_insert_tail( &use, q );
	new = l_get_struct( q, webser_t, queue );
	*webser = new;
	return OK;
}
// webser_free ----------------
static status webser_free( webser_t * webser )
{
	queue_remove( &webser->queue );
	queue_insert_tail( &usable, &webser->queue );

	webser->c = NULL;
	webser->data = NULL;

	webser->request_head = NULL;
	webser->request_body = NULL;

	webser->api_flag = 0;
	webser->api_handler = NULL;

	webser->filesize = 0;
	webser->re_status = 0;
	memset( webser->filepath, 0, sizeof(webser->filepath) );

	webser->ffd = 0;
	webser->response_head = NULL;
	webser->response_body = NULL;

	webser->upstream = NULL;
	return OK;
}
// webser_free_connection -------------------
static status webser_free_connection( event_t * ev )
{
	connection_t * c;

	c = ev->data;

	net_free( c );
	return OK;
}
// webser_close_connection -------------------
static status webser_close_connection( connection_t * c )
{
	int32 rc;

	if( c->ssl_flag && c->ssl ) {
		rc = ssl_shutdown( c->ssl );
		if( rc == AGAIN ) {
			c->ssl->handler = webser_free_connection;
			return AGAIN;
		}
	}
	webser_free_connection( c->write );
	return OK;
}
// webser_close ---------------------
static status webser_close( webser_t * webser )
{
	meta_t * meta, *cl;

	// l_safe_free headers_in mem_list
	if( webser->request_head ) {
		http_request_head_free( webser->request_head );
	}
	// request_body
	if( webser->request_body ) {
		http_entitybody_free( webser->request_body );
	}
	// response meta
	if( webser->ffd ) {
		close( webser->ffd );
	}
	meta = webser->response_head;
	while( meta ) {
		cl = meta->next;
		meta_free( meta );
		meta = cl;
	}
	// upstream
	if( webser->upstream ) {
		upstream_free( webser->upstream );
	}
	webser_free( webser );
	return OK;
}
// webser_time_out_connection --------------------------
static void webser_time_out_connection( void * data )
{
	connection_t * c;

	c = data;
	webser_close_connection( c );
}
// webser_time_out -------------------------------------------------------------
static void webser_time_out( void * data )
{
	webser_t * webser;
	connection_t * c;

	webser = data;
	c = webser->c;
	webser_close( webser );
	webser_close_connection( c );
}
// webser_keepalive --------------------
static status webser_keepalive( webser_t * webser )
{
	connection_t* c;
	int32 rc;
	event_t * read;
	uint32 busy_length = 0;
	webser_t * new;

	c = webser->c;
	read = c->read;
	if( webser->request_head->body_type == HTTP_ENTITYBODY_NULL ) {
		busy_length = meta_len( c->meta->pos, c->meta->last );
		if( busy_length ) {
			memcpy( c->meta->start, c->meta->pos, busy_length );
		}
	}
	c->meta->last = c->meta->pos = c->meta->start + busy_length;
	webser_close( webser );

	if( OK != webser_alloc( &new ) ) {
		err_log( "%s --- webser_alloc", __func__ );
		return ERROR;
	}
	new->c = c;
	c->data = (void*)new;
	if( OK != http_request_head_create( c, &new->request_head ) ) {
		err_log("%s --- request create", __func__ );
		webser_over( new );
		return ERROR;
	}
	c->write->handler = NULL;
	c->read->handler = webser_process_request;
	rc = c->read->handler( c->read );
	if( rc == AGAIN ) {
		c->read->timer.data = (void*)new;
		c->read->timer.handler = webser_time_out;
		timer_add( &c->read->timer, WEBSER_TIMEOUT );
	} else if ( rc == ERROR ) {
		webser_over( new );
	}
	return rc;
}
// webser_over ------
status webser_over( webser_t * webser )
{
	webser_close_connection( webser->c );
	webser_close( webser );
	return OK;
}
// webser_over_early --------------------
static status webser_over_early( webser_t * webser, uint32 status_code)
{
	connection_t * c;
	string_t * string;
	meta_t * meta = NULL;

	c = webser->c;
	if( webser->re_status == 400 ) {
		string = &response_string_400;
	} else if ( webser->re_status == 403 ) {
		string = &response_string_403;
	} else if ( webser->re_status == 404 ) {
		string = &response_string_404;
	} else {
		err_log( "%s --- status code [%d] not support yet", __func__,
		status_code );
		webser_over( webser );
		return ERROR;
	}
	meta = (meta_t*)l_safe_malloc( sizeof(meta_t) );
	if( !meta ) {
		err_log("%s --- l_safe_malloc meta", __func__ );
		webser_over( webser );
		return ERROR;
	}
	memset( meta, 0, sizeof(meta_t) );
	meta->next = NULL;
	meta->start = meta->pos = string->data;
	meta->last = meta->end = string->data + string->len;

	webser->response_head = meta;
	c->read->handler = webser_response;
	return c->read->handler( c->read );
}
// webser_send_response ----------------------------------------------------
static status webser_send_response( event_t * ev )
{
	status rc;
	connection_t * c;
	webser_t * webser;
	uint32 read_length;
	meta_t * cl = NULL;

	c = ev->data;
	webser = c->data;
	while( 1 ) {
		rc = c->send_chain( c, webser->response_head );
		if( rc == ERROR ) {
			err_log( "%s --- send response", __func__ );
			webser_over( webser );
			return ERROR;
		} else if( rc == DONE ) {
			cl = webser->response_body;
			if( cl ) {
				cl->file_pos += meta_len( cl->start, cl->pos );
				if( cl->file_pos > cl->file_last ) {
					err_log("%s --- file pos > file last", __func__ );
					webser_over( webser );
					return ERROR;
				}
				if( cl->file_pos < cl->file_last ) {
					cl->last = cl->pos = cl->start;
					((cl->file_last - cl->file_pos) > WEBSER_BODY_META_LENGTH )?
					( read_length = WEBSER_BODY_META_LENGTH ):
					( read_length = ( cl->file_last - cl->file_pos) );
					if( ERROR == read( webser->ffd, cl->last, read_length ) ) {
						err_log( "%s --- read file data, errno [%d]",
						__func__, errno );
						webser_over( webser );
						return ERROR;
					}
					cl->last = cl->pos + read_length;
					continue;
				}
			}
			timer_del( &c->write->timer );
			debug_log ( "%s --- success", __func__ );
			if( webser->re_status == 200 &&
				conf.http_keepalive &&
				webser->request_head->keepalive_flag ) {
				return webser_keepalive( webser );
			}
			return webser_over( webser );
		}
		c->write->timer.data = (void*)webser;
		c->write->timer.handler = webser_time_out;
		timer_add( &c->write->timer, WEBSER_TIMEOUT );
		return rc;
	}
}
// webser_test_reading ---------------------
static status webser_test_reading( event_t * ev  )
{
	char buf[1];
	connection_t * c;
	webser_t * webser;
	socklen_t len;
	int err;
	ssize_t n;

	c = ev->data;
	webser = c->data;

	len = sizeof(int);
	if( getsockopt( c->fd, SOL_SOCKET, SO_ERROR, (void*)&err, &len ) == -1 ) {
		err = errno;
	}
	goto closed;

	n = recv( c->fd, buf, 1, MSG_PEEK );
	if( n == -1 ) {
		err_log( "%s --- recv errno [%d]", __func__, errno );
		goto closed;
	} else if ( n == 0 ) {
		err_log( "%s --- client close", __func__ );
		goto closed;
	}
	return OK;

closed:
	webser_close( webser );
	webser_close_connection( c );
	return OK;
}
// webser_response ----------------------------------------
status webser_response( event_t * ev )
{
	connection_t* c;
	webser_t * webser;
	meta_t * cl;

	c = ev->data;
	webser = c->data;

	if( webser->response_body ) {
		for( cl = webser->response_body; cl; cl = cl->next ) {
			if( cl->file_pos == 0 && cl->file_last == 0 ) {
				cl->file_last = meta_len( cl->pos, cl->last );
			}
		}
		webser->response_head->next = webser->response_body;
	}
	c->read->handler = webser_test_reading;
	c->write->handler = webser_send_response;
	event_opt( c->write, EVENT_WRITE );
	return c->write->handler( c->write );
}
// webser_entity_body --------------------------------------
static status webser_entity_body( webser_t * webser )
{
	uint32 read_length = 0;

	webser->ffd = open( webser->filepath, O_RDONLY );
	if( webser->ffd == ERROR ) {
		err_log( "%s --- open request file, errno [%d]", __func__, errno );
		return ERROR;
	}
	if( !webser->c->ssl_flag && (webser->filesize > WEBSER_BODY_META_LENGTH) ) {
		if( OK != meta_file_alloc( &webser->response_body, webser->filesize ) ){
			err_log("%s --- meta file alloc", __func__ );
			return ERROR;
		}
	} else {
		read_length = l_min( webser->filesize, WEBSER_BODY_META_LENGTH );
		if( OK != meta_alloc( &webser->response_body, read_length ) ) {
			err_log( "%s --- meta alloc response_body", __func__ );
			return ERROR;
		}
		webser->response_body->file_pos = 0;
		webser->response_body->file_last = webser->filesize;
		/*
			did't adjust file offset yet, only can sequential reading
		*/
		if( ERROR == read( webser->ffd, webser->response_body->last,
			read_length ) ) {
			err_log( "%s --- read request file, errno [%d]", __func__, errno );
			return ERROR;
		}
		webser->response_body->last = webser->response_body->pos + read_length;
	}
	return OK;
}
// webser_entity_head --------------------------------------------------
status webser_entity_head( webser_t * webser )
{
	string_t * mimetype = NULL;
	char * ptr;
	char str[1024] = {0};
	uint32 head_len = 0;

	head_len += l_strlen("HTTP/1.1 200 OK\r\n");
	mimetype = webser_get_mimetype( webser );
	if( !mimetype ) {
		mimetype = &default_mimetype;
	}
	head_len += mimetype->len;
	snprintf( str, sizeof(str), "Content-Length: %d\r\n", webser->filesize );
	head_len += l_strlen(str);
	head_len += l_strlen("Server: lk-web-v1\r\n");
	head_len += l_strlen("Accept-Charset: utf-8\r\n");

	if( webser->request_head->headers_in.connection != NULL ) {
		if( (uint32)webser->request_head->headers_in.connection->len >
		l_strlen("close") ) {
			head_len += l_strlen("Connection: keep-alive\r\n");
		} else {
			head_len += l_strlen("Connection: close\r\n");
		}
	} else {
		head_len += l_strlen("Connection: close\r\n");
	}
	head_len += l_strlen("\r\n");

	if( OK != meta_alloc( &webser->response_head, head_len ) ) {
		err_log( "%s --- meta_alloc response_head", __func__ );
		return ERROR;
	}
	ptr = webser->response_head->data;
	memcpy( ptr, "HTTP/1.1 200 OK\r\n", l_strlen("HTTP/1.1 200 OK\r\n") );
	ptr += l_strlen("HTTP/1.1 200 OK\r\n");
	memcpy( ptr, "Server: lk-web-v1\r\n", l_strlen("Server: lk-web-v1\r\n") );
	ptr += l_strlen("Server: lk-web-v1\r\n");
	memcpy( ptr, "Accept-Charset: utf-8\r\n",
	l_strlen("Accept-Charset: utf-8\r\n") );
	ptr += l_strlen("Accept-Charset: utf-8\r\n");
	if( webser->request_head->headers_in.connection != NULL ) {
		if( (uint32)webser->request_head->headers_in.connection->len >
		l_strlen("close") ) {
			memcpy( ptr, "Connection: keep-alive\r\n",
			l_strlen("Connection: keep-alive\r\n") );
			ptr += l_strlen("Connection: keep-alive\r\n");
		} else {
			memcpy( ptr, "Connection: close\r\n",
			l_strlen("Connection: close\r\n") );
			ptr += l_strlen("Connection: close\r\n");
		}
	} else {
		memcpy( ptr, "Connection: close\r\n",l_strlen("Connection: close\r\n"));
		ptr += l_strlen("Connection: close\r\n");
	}
	memcpy( ptr, mimetype->data, mimetype->len );
	ptr += mimetype->len;
	memcpy( ptr, str, l_strlen(str) );
	ptr += l_strlen(str);
	memcpy( ptr, "\r\n", l_strlen("\r\n") );
	ptr += l_strlen("\r\n");

	webser->response_head->last += head_len;
	return OK;
}
// webser_entity_start -----------------------------------------------
static status webser_entity_start ( webser_t * webser )
{
	struct stat st;
	char * ptr;
	uint32 length;

	if( webser->request_head->method.len != l_strlen("GET") ||
 	strncmp( webser->request_head->method.data, "GET", l_strlen("GET") )!= 0 ) {
		err_log("%s --- method not 'GET'", __func__ );
		webser->re_status = 400;
		return OK;
	}
	// rewrite path
	ptr = webser->filepath;
	length = ( conf.home.data[conf.home.len-1] == '/' ) ? conf.home.len - 1 :
	conf.home.len;
	memcpy( ptr, conf.home.data, length );
	ptr += length;

	memcpy( ptr, webser->request_head->uri.data, webser->request_head->uri.len);
	ptr += webser->request_head->uri.len;

	if( webser->request_head->uri.data[webser->request_head->uri.len-1] =='/') {
		memcpy( ptr, conf.index.data, conf.index.len );
		ptr += conf.index.len;
	}
	// end of rewrite path
	if( OK != stat( webser->filepath, &st ) ) {
		err_log("%s --- stat request file, errno [%d]", __func__, errno );
		webser->re_status = 400;
		return OK;
	}
	if( S_ISREG(st.st_mode) ) {
		webser->re_status = ( S_IRUSR&st.st_mode ) ? 200 : 403;
	} else {
		webser->re_status = 404;
	}
	webser->filesize = ( webser->re_status == 200 ) ? (uint32)st.st_size : 0;
	return OK;
}
// webser_process_entity -----
static status webser_process_entity ( event_t * ev )
{
	connection_t * c;
	webser_t * webser;

	c = ev->data;
	webser = c->data;

	if( OK != webser_entity_start( webser ) ) {
		err_log( "%s --- entity start", __func__ );
		webser_over( webser );
		return ERROR;
	}
	if( webser->re_status != 200 ) {
		return webser_over_early( webser, webser->re_status );
	}
	if( OK != webser_entity_head( webser ) ) {
		err_log( "%s --- entity head", __func__ );
		webser_over( webser );
		return ERROR;
	}
	if( OK != webser_entity_body( webser ) ) {
		err_log( "%s --- entity body", __func__ );
		webser_over( webser );
		return ERROR;
	}
	c->read->handler = webser_response;
	return c->read->handler( c->read );
}
// webser_process_api ----------------------
static status webser_process_api( event_t * ev )
{
	int32 rc;
	connection_t * c;
	webser_t * webser;

	c = ev->data;
	webser = c->data;
	return webser->api_handler( (void*)webser );
}
// webser_process_request_body ----------
static status webser_process_request_body( event_t * ev )
{
	int32 status;
	connection_t * c;
	webser_t * webser;

	c = ev->data;
	webser = c->data;
	status = webser->request_body->handler( webser->request_body );
	if( status == ERROR ) {
		err_log( "%s --- get webser body", __func__ );
		webser_over( webser );
		return ERROR;
	} else if( status == DONE ) {
		timer_del( &c->read->timer );
		c->read->handler = webser->api_flag ?
		webser_process_api : webser_process_entity;
		return c->read->handler( c->read );
	}
	c->read->timer.data = (void*)webser;
	c->read->timer.handler = webser_time_out;
	timer_add( &c->read->timer, WEBSER_TIMEOUT );
	return status;
}
// webser_process ------------------
static status webser_process( event_t * ev )
{
	connection_t * c;
	webser_t * webser;
	int32 content_length;

	c = ev->data;
	webser = c->data;
	if( OK == serv_api_find( &webser->request_head->uri,
		&webser->api_handler ) ) {
		webser->api_flag = 1;
	} else {
		webser->api_flag = 0;
	}

	if( webser->request_head->body_type == HTTP_ENTITYBODY_NULL ) {
		c->read->handler = webser->api_flag ?
			webser_process_api : webser_process_entity;
	} else {
		if( OK != http_entitybody_create( webser->c, &webser->request_body ) ) {
			err_log( "%s --- http_entitybody_create error", __func__ );
			webser_over( webser );
			return ERROR;
		}
		webser->request_body->cache = webser->api_flag ? 1 : 0;
		webser->request_body->body_type = webser->request_head->body_type;
		if( webser->request_head->body_type == HTTP_ENTITYBODY_CONTENT ) {
			webser->request_body->content_length =
				webser->request_head->content_length;
		}
		c->read->handler = webser_process_request_body;
	}
	return c->read->handler( c->read );
}
// webser_process_request -------
static status webser_process_request( event_t * ev )
{
	connection_t * c;
	webser_t * webser;
	status rc;

	c = ev->data;
	webser = c->data;
	rc = webser->request_head->handler( webser->request_head );
	if( rc == ERROR ) {
		err_log("%s --- request", __func__ );
		webser_over( webser );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &c->read->timer );
		c->read->handler = webser_process;
		return c->read->handler( c->read );
	}
	c->read->timer.data = (void*)webser;
	c->read->timer.handler = webser_time_out;
	timer_add( &c->read->timer, WEBSER_TIMEOUT );
	return AGAIN;
}
// webser_start_connection ----------------------------------
static status webser_start_connection( event_t * ev )
{
	connection_t * c;
	webser_t * new;

	c = ev->data;
	if( !c->meta ) {
		if( OK != meta_alloc( &c->meta, 4096 ) ) {
			err_log( "%s --- c meta alloc", __func__ );
			webser_close_connection( c );
			return ERROR;
		}
	}
	if( OK != webser_alloc( &new ) ) {
		err_log( "%s --- webser_alloc", __func__ );
		return ERROR;
	}
	new->c = c;
	c->data = (void*)new;
	if( OK != http_request_head_create( c, &new->request_head ) ) {
		err_log("%s --- request create", __func__ );
		webser_over( new );
		return ERROR;
	}
	c->write->handler = NULL;
	c->read->handler = webser_process_request;
	return c->read->handler( c->read );
}
// webser_ssl_handshake_handler ----------------
static status webser_ssl_handshake_handler( event_t * ev )
{
	connection_t * c;

	c = ev->data;

	if( !c->ssl->handshaked ) {
		err_log( "%s --- handshake error", __func__ );
		webser_close_connection( c );
		return ERROR;
	}
	timer_del( &c->read->timer );
	c->recv = ssl_read;
	c->send = ssl_write;
	c->recv_chain = NULL;
	c->send_chain = ssl_write_chain;

	c->write->handler = NULL;
	c->read->handler = webser_start_connection;
	return c->read->handler( c->read );
}
// webser_init_connection ---------------------
static status webser_init_connection( event_t * ev )
{
	connection_t * c;
	status rc;

	c = ev->data;
	if( c->ssl_flag ) {
		if( OK != ssl_create_connection( c, L_SSL_SERVER ) ) {
			err_log( "%s --- ssl create", __func__ );
			webser_close_connection( c );
			return ERROR;
		}
		rc = ssl_handshake( c->ssl );
		if( rc == ERROR ) {
			err_log( "%s --- ssl handshake", __func__ );
			webser_close_connection( c );
			return ERROR;
		} else if ( rc == AGAIN ) {
			c->read->timer.data = (void*)c;
			c->read->timer.handler = webser_time_out_connection;
			timer_add( &c->read->timer, WEBSER_TIMEOUT );

			c->ssl->handler = webser_ssl_handshake_handler;
			return AGAIN;
		}
		return webser_ssl_handshake_handler( ev );
	}
	c->read->handler = webser_start_connection;

	c->read->timer.data = (void*)c;
	c->read->timer.handler = webser_time_out_connection;
	timer_add( &c->read->timer, WEBSER_TIMEOUT );
	return AGAIN;
}
// webser_process_init ----------
status webser_process_init( void )
{
	uint32 i = 0;

	queue_init( &use );
	queue_init( &usable );
	pool = ( webser_t *) l_safe_malloc( sizeof(webser_t) * MAXCON );
	if( !pool ) {
		err_log( "%s --- l_safe_malloc pool", __func__ );
		return ERROR;
	}
	memset( pool, 0, sizeof(webser_t) * MAXCON );
	for( i = 0; i < MAXCON; i ++ ) {
		queue_insert_tail( &usable, &pool[i].queue );
	}
	return OK;
}
// webser_process_end ----------
status webser_process_end( void )
{
	if( pool ) {
		l_safe_free( pool );
		pool = NULL;
	}
	return OK;
}
// webser_init -------------
status webser_init( void )
{
	uint32 i = 0;
	for( i = 0; i < conf.http_n; i ++ ) {
		listen_add( conf.http[i], webser_init_connection, HTTP );
	}
	for( i = 0; i < conf.https_n; i ++ ) {
		listen_add( conf.https[i], webser_init_connection, HTTPS );
	}
	return OK;
}
// webser_end ----------
status webser_end( void )
{
	return OK;
}

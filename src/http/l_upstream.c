#include "lk.h"

// upstream_test_reading ---------------------
static status upstream_test_reading( event_t * ev  )
{
	char buf[1];
	connection_t * c;
	webser_t * webser;
	socklen_t len;
	int32 err;
	ssize_t n;

	c = ev->data;
	webser = c->data;

	len = sizeof(errno);
	if( getsockopt( c->fd, SOL_SOCKET, SO_ERROR, (void*)&err, &len ) == -1 ) {
		err = errno;
	}
	goto closed;

	n = recv( c->fd, buf, 1, MSG_PEEK );
	if( n == -1 ) {
		err_log ( "%s --- recv errno [%d]", __func__, errno );
		goto closed;
	} else if ( n == 0 ) {
		err_log ( "%s --- client close", __func__ );
		goto closed;
	}
	return OK;

closed:
	debug_log ( "%s --- downstream closed", __func__ );
	return webser_over( webser );
}
// upstream_alloc ---------------------------------
status upstream_alloc( upstream_t ** up )
{
	upstream_t * new = NULL;

	new = (upstream_t*)l_safe_malloc( sizeof( upstream_t ));
	if( !new ) {
		err_log ( "%s --- new null", __func__ );
		return ERROR;
	}
	memset( new, 0, sizeof(upstream_t) );

	*up = new;
	return OK;
}
// upstream_free_connection ----------------------
static status upstream_free_connection( event_t * ev )
{
	connection_t * c;
	upstream_t * up;

	c = ev->data;
	up = c->data;

	if( up->upstream ) {
		net_free( up->upstream );
		up->upstream = NULL;
	}
	l_safe_free( up );
	return OK;
}
// upstream_free -------------------------
status upstream_free( upstream_t * up )
{
	int32 rc;

	if( up->request_meta ) {
		meta_free( up->request_meta );
	}
	if( up->transport ) {
		net_transport_free( up->transport );
	}
	up->transport = NULL;
	up->request_meta = NULL;
	up->handler = NULL;

	if( up->upstream->ssl_flag && up->upstream->ssl ) {
		rc = ssl_shutdown( up->upstream->ssl );
		if( rc == AGAIN ) {
			up->upstream->ssl->handler = upstream_free_connection;
			return AGAIN;
		}
	}
	upstream_free_connection( up->upstream->write );
	return OK;
}
// upstream_over ---------------
static status upstream_over( upstream_t * up )
{
	webser_over( (webser_t*)up->downstream->data );
	return OK;
}
// upstream_time_out -----------
static void upstream_time_out( void * data )
{
	upstream_t * up;

	up = data;
	debug_log ( "%s --- ", __func__ );
	upstream_over( up );
}
// upstream_recv_upstream ----------------------
static status upstream_recv_upstream( event_t * ev )
{
	upstream_t * up;
	connection_t * c;
	status rc;

	c = ev->data;
	up = c->data;

	up->upstream->read->timer.data = (void*)up;
	up->upstream->read->timer.handler = upstream_time_out;
	timer_add( &up->upstream->read->timer, UP_TIMEOUT );

	rc = net_transport( up->transport, 0 );
	if( rc == ERROR ) {
		return upstream_over( up );
	}
	return rc;
}
// upstream_send_downstream --------------------
static status upstream_send_downstream( event_t * event )
{
	webser_t * webser;
	upstream_t * up;
	connection_t * c;
	status rc;

	c = event->data;
	webser = c->data;
	up = webser->upstream;

	rc = net_transport( up->transport, 1 );
	if( rc == ERROR ) {
		return upstream_over( up );
	}
	return rc;
}
// upstream_send_webser_handler -------------------------
static status upstream_send_webser_handler( event_t * ev )
{
	int32 rc;
	upstream_t * up;
	connection_t * c;

	c = ev->data;
	up = c->data;

	rc = up->upstream->send_chain( up->upstream, up->request_meta );
	if( rc == ERROR ) {
		err_log ( "%s --- send error, errno [%d]", __func__, errno );
		upstream_over( up );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &up->upstream->write->timer );
		debug_log("%s --- success", __func__ );

		if( OK != net_transport_alloc( &up->transport ) ) {
			err_log ( "%s --- transport alloc", __func__ );
			upstream_over( up );
			return ERROR;
		}
		up->transport->recv_connection = up->upstream;
		up->transport->send_connection = up->downstream;

		event_opt( up->upstream->read, EVENT_READ );
		up->upstream->write->handler = NULL;
		up->upstream->read->handler = upstream_recv_upstream;
		up->downstream->write->handler = upstream_send_downstream;

		return up->upstream->read->handler( up->upstream->read );
	}
	up->upstream->write->timer.data = (void*)up;
	up->upstream->write->timer.handler = upstream_time_out;
	timer_add( &up->upstream->write->timer, UP_TIMEOUT );
	return rc;
}
// upstream_make_webser ---------------------------
static status upstream_make_webser( upstream_t * up, meta_t ** meta )
{
	size_t len = 0;
	meta_t * new;
	char * ptr;

	//keep-alive
	//close
	len += l_strlen("GET ");
	len += up->upstream_send.uri.len;
	len += l_strlen(" HTTP/1.1\r\n");

	len += l_strlen("Host: ");
	len += up->upstream_send.host.len;
	len += l_strlen("\r\n");

	len += l_strlen("Upgrade-Insecure-Requests: 1\r\n");
	len += l_strlen("Connection: close\r\n");
	len += l_strlen("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/64.0.3282.186 Safari/537.36\r\n");
	len += l_strlen("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n");
	len += l_strlen("Accept-Encoding: gzip, deflate\r\n");
	len += l_strlen("Accept-Language: zh-CN,zh;q=0.9\r\n");
	len += l_strlen("\r\n");

	if( OK != meta_alloc( &new, (uint32)len ) ) {
		err_log ( "%s --- l_safe_malloc meta new", __func__ );
		return ERROR;
	}

	ptr = (char*)new->data;

	memcpy( ptr, "GET ", l_strlen("GET ") );
	ptr += l_strlen("GET ");
	memcpy( ptr, up->upstream_send.uri.data, up->upstream_send.uri.len );
	ptr += up->upstream_send.uri.len;
	memcpy( ptr, " HTTP/1.1\r\n", l_strlen(" HTTP/1.1\r\n") );
	ptr += l_strlen(" HTTP/1.1\r\n");

	memcpy( ptr, "Host: ", l_strlen("Host: ") );
	ptr += l_strlen("Host: ");
	memcpy( ptr, up->upstream_send.host.data, up->upstream_send.host.len );
	ptr += up->upstream_send.host.len;
	memcpy( ptr, "\r\n", l_strlen("\r\n") );
	ptr += l_strlen("\r\n");

	memcpy( ptr, "Upgrade-Insecure-Requests: 1\r\n",
	l_strlen("Upgrade-Insecure-Requests: 1\r\n") );
	ptr += l_strlen("Upgrade-Insecure-Requests: 1\r\n");

	memcpy( ptr, "Connection: close\r\n",
	l_strlen("Connection: close\r\n") );
	ptr += l_strlen("Connection: close\r\n");

	memcpy( ptr, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/64.0.3282.186 Safari/537.36\r\n",
	l_strlen("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/64.0.3282.186 Safari/537.36\r\n") );
	ptr += l_strlen("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/64.0.3282.186 Safari/537.36\r\n");

	memcpy( ptr, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n",
	l_strlen("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n") );
	ptr += l_strlen("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8\r\n");

	memcpy( ptr, "Accept-Encoding: gzip, deflate\r\n",
	l_strlen("Accept-Encoding: gzip, deflate\r\n") );
	ptr += l_strlen("Accept-Encoding: gzip, deflate\r\n");

	memcpy( ptr, "Accept-Language: zh-CN,zh;q=0.9\r\n",
	l_strlen("Accept-Language: zh-CN,zh;q=0.9\r\n") );
	ptr += l_strlen("Accept-Language: zh-CN,zh;q=0.9\r\n");

	memcpy( ptr, "\r\n", l_strlen("\r\n") );
	ptr += l_strlen("\r\n");

	new->last += len;

	//debug_log ( "%.*s", new->last - new->pos, new->pos );
	*meta = new;
	return OK;
}
// upstream_send_webser ---------------
static status upstream_send_webser( event_t * ev )
{
	connection_t * c;
	upstream_t * up;
	meta_t * meta;

	c = ev->data;
	up = c->data;

	debug_log("%s --- ", __func__ );
	if( OK != upstream_make_webser( up, &meta ) ) {
		err_log ( "%s --- upstream_make_webser", __func__ );
		upstream_over( up );
		return ERROR;
	}

	up->request_meta = meta;
	// add webser body part
	if( up->upstream_send.body ) {
		up->request_meta->next = up->upstream_send.body;
	}

	up->upstream->write->handler = upstream_send_webser_handler;
	return up->upstream->write->handler( up->upstream->write );
}
// upstream_ssl_handshake_handler ------------------------
static status upstream_ssl_handshake_handler( event_t * ev )
{
	connection_t * upstream;
	upstream_t * up;

	upstream = ev->data;
	up = upstream->data;

	if( !upstream->ssl->handshaked ) {
		err_log( "%s --- handshake error", __func__ );
		upstream_over( up );
		return ERROR;
	}
	timer_del( &up->upstream->write->timer );

	up->upstream->recv = ssl_read;
	up->upstream->send = ssl_write;
	up->upstream->recv_chain = NULL;
	up->upstream->send_chain = ssl_write_chain;

	up->upstream->read->handler = NULL;
	up->upstream->write->handler = upstream_send_webser;
	return up->upstream->write->handler( up->upstream->write );
}
// upstream_test_connect ------
static status upstream_test_connect( connection_t * c )
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
// upstream_connect_check -----------------------
static status upstream_connect_check( event_t * ev )
{
	status rc;
	connection_t * c;
	upstream_t * up;

	c = ev->data;
	up = c->data;

	if( OK != upstream_test_connect( up->upstream ) ) {
		err_log ( "%s --- connect error", __func__ );
		upstream_over( up );
		return ERROR;
	}
	timer_del( &ev->timer );
	debug_log ( "%s --- connect success", __func__, ev );

	if( up->upstream->ssl_flag ) {
		if( OK != ssl_create_connection( up->upstream, L_SSL_CLIENT ) ) {
			err_log ( "%s --- net ssl create", __func__ );
			upstream_over( up );
			return ERROR;
		}
		rc = ssl_handshake( up->upstream->ssl );
		if( rc == ERROR ) {
			err_log ( "%s --- handshake error", __func__ );
			upstream_over( up );
			return ERROR;
		} else if ( rc == AGAIN ) {
			up->upstream->write->timer.data = (void*)up;
			up->upstream->write->timer.handler = upstream_time_out;
			timer_add( &up->upstream->write->timer, UP_TIMEOUT );

			up->upstream->ssl->handler = upstream_ssl_handshake_handler;
			return AGAIN;
		}
		return upstream_ssl_handshake_handler( ev );
	}
	up->upstream->write->handler = upstream_send_webser;
	return upstream_send_webser( ev );
}
// upstream_connect ---------------
static status upstream_connect ( event_t * ev )
{
	status rc;
	upstream_t * up;
	connection_t * c;

	c = ev->data;
	up = c->data;

	rc = event_connect( up->upstream->write );
	if( rc == ERROR ) {
		err_log ( "%s --- connect error", __func__ );
		upstream_over( up );
		return ERROR;
	}
	event_opt( up->upstream->write, EVENT_WRITE );
	up->upstream->write->handler = upstream_connect_check;

	if( rc == AGAIN ) {
		up->upstream->write->timer.data = (void*)up;
		up->upstream->write->timer.handler = upstream_time_out;
		timer_add( &up->upstream->write->timer, UP_TIMEOUT );

		debug_log ( "%s --- connect again", __func__, up->upstream->write );
		return AGAIN;
	}
	return up->upstream->write->handler( up->upstream->write );
}
// upstream_init --------------------------------
static status upstream_init( upstream_t * up )
{
	timer_del( &up->downstream->write->timer );
	timer_del( &up->downstream->read->timer );

	up->downstream->read->handler = upstream_test_reading;
	up->downstream->write->handler = NULL;

	up->upstream->read->handler = NULL;
	up->upstream->write->handler = upstream_connect;
	return up->upstream->write->handler( up->upstream->write );
}
// upstream_create -------------------------------
static status upstream_create( upstream_t ** up )
{
	upstream_t * new;

	if( OK != upstream_alloc(&new) ) {
		debug_log ( "%s --- upstream alloc", __func__ );
		upstream_free( new );
		return ERROR;
	}
	if( OK != net_alloc( &new->upstream ) ) {
		err_log ( "%s --- net alloc", __func__ );
		upstream_free( new );
		return ERROR;
	}
	if( !new->upstream->meta ) {
		if( OK != meta_alloc( &new->upstream->meta, 4096 ) ) {
			err_log( "%s --- new->upstream meta alloc", __func__ );
			upstream_free( new );
			return ERROR;
		}
	} else {
		new->upstream->meta->pos =
		new->upstream->meta->last =
		new->upstream->meta->start;
	}
	new->upstream->data = (void*)new;

	new->upstream->send = sends;
	new->upstream->recv = recvs;
	new->upstream->send_chain = send_chains;
	new->upstream->recv_chain = NULL;
	*up = new;
	return OK;
}
// upstream_addr_ip ---------------------------------
static status upstream_addr_ip( upstream_t * up, string_t * ip, string_t * serv )
{
	struct addrinfo * res;

	memset( &up->upstream->addr, 0, sizeof(struct sockaddr_in) );
	res = net_get_addr( ip, serv );
	if( NULL == res ) {
		err_log ( "%s --- net_get_addr error", __func__ );
		return ERROR;
	}
	memset( &up->upstream->addr, 0, sizeof(struct sockaddr_in) );
	memcpy( &up->upstream->addr, res->ai_addr, sizeof(struct sockaddr_in) );
	freeaddrinfo( res );
	return OK;
}
// upstream_set_value -------------------------------
static status upstream_webser_value( upstream_t * up, string_t *url, string_t *host, meta_t * body, uint32 https )
{
	assert( url != NULL );
	assert( host != NULL );

	up->upstream_send.uri.data  = url->data;
	up->upstream_send.uri.len = url->len;

	up->upstream_send.host.data = host->data;
	up->upstream_send.host.len = host->len;

	if( body ) {
		up->upstream_send.body = body;
	}
	if( https ) {
		up->upstream->ssl_flag = 1;
	}
	return OK;
}
// upstream_info_get -------
static status upstream_info_get( upstream_t * up, json_t * json )
{
	string_t json_ip, json_port, json_host, json_uri;
	json_t *obj, *value;
	uint32 ssl_flag = 0;
	char str[1024] = {0};

	json_get_child( json, 1, &obj );
	// get ip
	if( OK != json_get_obj_str( obj, "ip", l_strlen("ip"), &value ) ) {
		err_log( "%s --- json 'ip' missing or not str", __func__ );
		return ERROR;
	}
	json_ip.data = value->name.data;
	json_ip.len = value->name.len;
	// get port
	if( OK != json_get_obj_num( obj, "port", l_strlen("port"), &value ) ) {
		err_log( "%s --- json 'port' missing or not num", __func__ );
		return ERROR;
	}
	snprintf( str, sizeof(str), "%d", (uint32)value->num );
	json_port.data = str;
	json_port.len = l_strlen(str);
	if( OK != upstream_addr_ip( up, &json_ip, &json_port ) ) {
		err_log( "%s --- upstream addr ip", __func__ );
		return ERROR;
	}
	// get https
	if( OK != json_get_obj_bool( obj, "https", l_strlen("https"), &value ) ) {
		err_log( "%s --- json 'https' missing or not bool", __func__ );
		return ERROR;
	}
	ssl_flag = ( value->type == JSON_TRUE ) ? 1 : 0;
	// get host
	if( OK != json_get_obj_str( obj, "host", l_strlen("host"), &value ) ) {
		err_log( "%s --- json 'host' missing or not str", __func__ );
		return ERROR;
	}
	json_host.data = value->name.data;
	json_host.len = value->name.len;
	// get uri
	if( OK != json_get_obj_str( obj, "uri", l_strlen("uri"), &value ) ) {
		err_log( "%s --- json 'uri' missing or not str", __func__ );
		return ERROR;
	}
	json_uri.data = value->name.data;
	json_uri.len = value->name.len;

	debug_log( "%s --- ip [%.*s]", __func__, json_ip.len, json_ip.data );
	debug_log( "%s --- port [%.*s]", __func__, json_port.len, json_port.data );
	debug_log( "%s --- https [%d]", __func__, ssl_flag );
	debug_log( "%s --- host [%.*s]", __func__, json_host.len, json_host.data );
	debug_log( "%s --- uri [%.*s]", __func__, json_uri.len, json_uri.data );

	if( OK != upstream_webser_value( up,
		&json_uri,
		&json_host,
		NULL,
		ssl_flag ) ) {
		err_log("%s --- upstream set value", __func__ );
		return ERROR;
	}
	return OK;
}
// upstream_start ----
status upstream_start( void * data, json_t * json )
{
	upstream_t * up;
	webser_t * webser;

	webser = data;

	if( OK != upstream_create( &up ) ) {
		err_log("%s --- upstream create", __func__ );
		return ERROR;
	}
	webser->upstream = up;
	up->downstream = webser->c;

	if( OK != upstream_info_get( up, json ) ) {
		err_log("%s --- upstream info get", __func__ );
		return ERROR;
	}
	return upstream_init( up );
}

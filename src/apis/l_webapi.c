#include "lk.h"

static status api_web_response_success( webser_t * webser );
static status api_web_response_failed( webser_t * webser, char * str, uint32 length );

static status api_perform_info( void * data );
static status api_perform_start( void * data );
static status api_perform_stop( void * data );
static status api_proxy( void * data );

// ---------
static serv_api_t api_arr_web[] = {
	{ string("/perform_info"),			api_perform_info },
	{ string("/perform_start"),			api_perform_start },
	{ string("/perform_stop"),			api_perform_stop },
	{ string("/proxy"),					api_proxy },
	{ string_null,						NULL }
};
// api_proxy ---------------
status api_proxy( void * data )
{
	webser_t * webser;
	meta_t *cl;
	json_t * json;
	status rc;

	webser = (webser_t *)data;
	if( webser->request_body && webser->request_body->body_head ) {
		if( webser->request_body->body_head->next ) {
			return api_web_response_failed( webser,
				"Proxy request too large",
				l_strlen("Proxy request too large") );
		}
		cl = webser->request_body->body_head;
		if( OK != json_decode( &json, cl->pos, cl->last ) ) {
			err_log(  "%s --- json decode error", __func__ );
			return api_web_response_failed( webser,
				"Json data error",
				l_strlen("Json data error") );
		}
		rc = upstream_start( (void*)webser, json );
		json_free( json );
		if( rc == ERROR ) {
			err_log("%s --- upstream start failed", __func__ );
			return api_web_response_failed( webser,
				"Proxy upstream failed",
				l_strlen("Proxy upstream failed") );
		}
		return rc;
	}
	return api_web_response_failed( webser,
		"Json empty",
		l_strlen("Json empty") );
}
// api_perform_info -------------------
status api_perform_info( void * data )
{
	webser_t * webser;
	json_t * json;
	meta_t * t;

	webser = data;
	if( !conf.perf_switch ) {
		err_log("%s --- perf off", __func__ );
		return api_web_response_failed( webser,
			"Perf off",
			l_strlen("Perf off") );
	}
	if( OK != performance_count_output( &json ) ) {
		err_log(  "%s --- performance_count_output", __func__ );
		return api_web_response_failed( webser,
			"Get count failed",
			l_strlen("Get count failed") );
	}
	if( OK != json_encode( json, &t ) ) {
		json_free( json );
		err_log(  "%s --- json encode", __func__ );
		return api_web_response_failed( webser,
			"Count json decode",
			l_strlen("Count json decode") );
	}
	json_free( json );

	webser_set_mimetype( webser, ".json", l_strlen(".json") );
	webser->re_status = 200;
	webser->filesize = meta_len( t->pos, t->last );
	webser_entity_head( webser );
	webser->response_body = t;

	return webser_response( webser->c->write );
}
// api_perform_setting_data_check ------------
static status api_perform_setting_data_check( json_t * json )
{
	json_t *t, *x, *v;
	uint32 i;
	string_t method;

	json_get_child( json, 1, &t );
	if( t->type != JSON_OBJ ) {
		err_log(  "%s --- root not obj", __func__ );
		return ERROR;
	}
	// check time
	if( OK != json_get_obj_num( t, "time", l_strlen("time"), &x ) ) {
		err_log(  "%s --- element missing 'time'", __func__ );
		return ERROR;
	}
	// check concurrent
	if( OK != json_get_obj_num( t, "concurrent", l_strlen("concurrent"), &x ) ) {
		err_log(  "%s --- element missing 'concurrent'", __func__ );
		return ERROR;
	}
	// check keep alive
	if( OK != json_get_obj_bool( t, "keepalive", l_strlen("keepalive"), &x ) ) {
		err_log(  "%s --- element missing 'keepalive'", __func__ );
		return ERROR;
	}
	// check pipeline array
	if( OK != json_get_obj_arr( t, "pipeline", l_strlen("pipeline"), &x ) ) {
		err_log(  "%s --- element missing 'pipeline'", __func__ );
		return ERROR;
	}
	if( x->list->elem_num < 1 ) {
		err_log(  "%s --- pipeline array empty", __func__ );
		return ERROR;
	}
	for( i = 1; i <= x->list->elem_num; i ++ ) {
		json_get_child( x, i, &t );
		// index part
		if( OK != json_get_obj_num( t, "index", l_strlen("index"), &v ) ) {
			err_log(  "%s --- array missing 'index'", __func__ );
			return ERROR;
		}
		// ip part
		if( OK != json_get_obj_str( t, "ip", l_strlen("ip"), &v ) ) {
			err_log(  "%s --- array missing 'ip'", __func__ );
			return ERROR;
		}
		// port part
		if( OK != json_get_obj_num( t, "port", l_strlen("port"), &v ) ) {
			err_log(  "%s --- array missing 'port", __func__ );
			return ERROR;
		}
		// https part
		if( OK != json_get_obj_bool( t, "https", l_strlen("https"), &v ) ) {
			err_log(  "%s --- array missing 'https'", __func__ );
			return ERROR;
		}
		// host part
		if( OK != json_get_obj_str( t, "host", l_strlen("host"), &v ) ) {
			err_log(  "%s --- array missing 'host'", __func__ );
			return ERROR;
		}
		// method part
		if( OK != json_get_obj_str( t, "method", l_strlen("method"), &v ) ) {
			err_log(  "%s --- array missing 'method'", __func__ );
			return ERROR;
		}
		method.data = v->name.data;
		method.len = v->name.len;
		// uri part
		if( OK != json_get_obj_str( t, "uri", l_strlen("uri"), &v ) ) {
			err_log(  "%s --- array missing 'uri'", __func__ );
			return ERROR;
		}
		// body part
		if( method.len == l_strlen("XXXX") ) {
			if( strncmp( method.data, "POST", method.len ) == 0 ||
			strncmp( method.data, "post", method.len ) ) {
				if( OK != json_get_child_by_name( t, "body",
				l_strlen("body"), &v ) ) {
					err_log("%s --- method post, have't 'body'", __func__ );
					return ERROR;
				}
			}
		}
	}
	return OK;
}
// api_perform_start --------------------
status api_perform_start( void * data )
{
	webser_t * webser;
	meta_t *cl, *t;
	uint32 length = 0, i = 0, single_body = 0;
	json_t * json;
	int32 perf_file;
	ssize_t status;

	webser = data;
	// check perf mode on/off
	if( !conf.perf_switch ) {
		err_log( "%s --- perf off", __func__ );
		return api_web_response_failed( webser,
			"Perf off",
			l_strlen("Perf off") );
	}
	// check if perf is running
	if( performance_process_running( ) ) {
		err_log( "%s --- perform is running", __func__ );
		return api_web_response_failed( webser,
			"Perf is running",
			l_strlen("Perf is running") );
	}
	// get request data
	if( !webser->request_body->body_head ) {
		err_log("%s --- have't request body", __func__ );
		return api_web_response_failed( webser,
			"Have't request body",
			l_strlen("Have't request body") );
	}
	if( webser->request_body->body_head->next ) {
		for( cl = webser->request_body->body_head; cl; cl = cl->next ) {
			length += meta_len( cl->pos, cl->last );
		}
		// limit json data length
		if( length > PERFORM_SETTING_LENGTH ) {
			err_log("%s --- json data length > PERFORM_SETTING_LENGTH", __func__ );
			return api_web_response_failed( webser,
				"Json data too large",
				l_strlen("Json data too large") );
		}
		if( OK != meta_alloc( &t, length ) ) {
			err_log("%s --- malloc meta for body meta chain", __func__ );
			return api_web_response_failed( webser,
				"Alloc body temp meta",
				l_strlen("Alloc body temp meta") );
		}
		for( cl = webser->request_body->body_head; cl; cl = cl->next ) {
			memcpy( t->last, cl->pos, meta_len( cl->pos, cl->last ) );
			t->last += meta_len( cl->pos, cl->last );
		}
	} else {
		t = webser->request_body->body_head;
		single_body = 1;
	}
	// check request data format
	if( OK != json_decode( &json, t->pos, t->last ) ) {
		if( !single_body ) meta_free( t );
		err_log( "%s --- json decode", __func__ );
		return api_web_response_failed( webser,
			"Json data error",
			l_strlen("Json data error") );
	}
	if( OK != api_perform_setting_data_check( json ) ) {
		json_free( json );
		if( !single_body ) meta_free( t );
		err_log("%s --- json data check failed", __func__ );
		return api_web_response_failed( webser,
			"Json data error",
			l_strlen("Json data error") );
	}
	json_free( json );
	// perform setting data save
	perf_file = open( L_PATH_PERFTEMP, O_CREAT|O_RDWR|O_TRUNC, 0644 );
	if( perf_file == ERROR ) {
		if( !single_body ) meta_free( t );
		err_log("%s --- perf temp file open", __func__ );
		return api_web_response_failed( webser,
			"Perf cache file open failed",
			l_strlen("Perf cache file open failed") );;
	}
	status = write( perf_file, t->pos, meta_len( t->pos, t->last ) );
	if( status == ERROR ) {
		if( !single_body ) meta_free( t );
		close( perf_file );
		err_log("%s --- write pid to perf_file", __func__ );
		return api_web_response_failed( webser,
			"Perf cache file write failed",
			l_strlen("Perf cache file write failed") );;
	}
	if( !single_body ) meta_free( t );
	close( perf_file );
	// set signal, prepare goto perform test
	sig_perf = 1;
	if( process_id != 0xffff ) {
		// if have worker process
		for( i = 0; i < process_id; i ++ ) {
			if( ERROR == kill( process_arr[i].pid, SIGUSR1 ) ) {
				err_log( "%s --- kill perf signal, [%d]", __func__, errno );
				return api_web_response_failed( webser,
					"Perf message send failed",
					l_strlen("Perf message send failed") );;
			}
		}
	}
	return api_web_response_success( webser );
}
// api_perform_stop -----------
status api_perform_stop( void * data )
{
	webser_t * webser;
	uint32 i;

	webser = data;
	if( !conf.perf_switch ) {
		err_log( "%s --- perf off", __func__ );
		return api_web_response_failed( webser,
			"Perf off",
			l_strlen("Perf off") );
	}
	if( !performance_process_running( ) ) {
		err_log( "%s --- perf not running", __func__ );
		return api_web_response_failed( webser,
			"Perf not running",
			l_strlen("Perf not running") );
	}
	sig_perf_stop = 1;
	if( process_id != 0xffff ) {
		for( i = 0; i < process_id; i ++ ) {
			if( ERROR == kill( process_arr[i].pid, SIGUSR2 ) ) {
				err_log( "%s --- kill perf stop signal, [%d]", __func__, signal );
				return api_web_response_failed( webser,
					"Perf stop message send failed",
					l_strlen("Perf stop message send failed") );;
			}
		}
	}
	return api_web_response_success( webser );
}
// api_web_response_success ----
static status api_web_response_success( webser_t * webser )
{
	/*
	{ "status":"success" }
	*/
	string_t str = string("{\"status\":\"success\"}");
	meta_t * t;

	meta_alloc( &t, str.len );
	memcpy( t->last, str.data, str.len );
	t->last += str.len;

	webser_set_mimetype( webser, ".json", l_strlen(".json") );
	webser->re_status = 200;
	webser->filesize = str.len;
	webser_entity_head( webser );
	webser->response_body = t;

	return webser_response( webser->c->write );
}
// api_web_response_failed ----
static status api_web_response_failed( webser_t * webser, char * str, uint32 length )
{
	/*
	{"status":"error","resson":"xxx"}
	*/
	string_t str_head = string("{\"status\":\"error\",\"reason\":\"");
	string_t str_tail = string("\"}");
	meta_t * t;
	uint32 all_length;

	all_length = length + str_head.len + str_tail.len;
	meta_alloc( &t, all_length );

	memcpy( t->last, str_head.data, str_head.len );
	t->last += str_head.len;

	memcpy( t->last, str, length );
	t->last += length;

	memcpy( t->last, str_tail.data, str_tail.len );
	t->last += str_tail.len;

	webser_set_mimetype( webser, ".json", l_strlen(".json") );
	webser->re_status = 200;
	webser->filesize = all_length;
	webser_entity_head( webser );
	webser->response_body = t;

	return webser_response( webser->c->write );
}
// webapi_init -----------
status webapi_init( void )
{
    uint32 i;

    for( i = 0; api_arr_web[i].handler; i ++ ) {
		serv_api_register( &api_arr_web[i] );
	}
    return OK;
}
// webapi_end -----------
status webapi_end( void )
{
    return OK;
}

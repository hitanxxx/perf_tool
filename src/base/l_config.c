#include "lk.h"

config_t conf;

// config_get ----
status config_get ( meta_t ** meta, char * path )
{
	int fd;
	struct stat stconf;
	uint32 length;
	meta_t * new;

	if( stat( path, &stconf ) < 0 ) {
		err_log( "%s --- config.json not found", __func__  );
		return ERROR;
	}
	length = (uint32)stconf.st_size;
	if( length > CONF_SETTING_LENGTH ) {
		err_log( "%s --- config.json > CONF_SETTING_LENGTH" );
		return ERROR;
	}
	if( OK != meta_alloc( &new, length ) ) {
		err_log("%s --- meta alloc", __func__ );
		return ERROR;
	}
	fd = open( path, O_RDONLY );
	if( ERROR == fd ) {
		err_log("%s --- can't open config.json", __func__ );
		meta_free( new );
		return ERROR;
	}
	if( ERROR == read( fd, new->last, length ) ) {
		err_log("%s --- read config.json", __func__ );
		meta_free( new );
		close( fd );
		return ERROR;
	}
	new->last += length;
	close(fd);
	*meta = new;
	return OK;
}
// config_parse_global ----------
static status config_parse_global( json_t * json )
{
	char str[1024] = {0};
	json_t *root_obj, *v;

	json_get_child( json, 1, &root_obj );
	if( OK == json_get_obj_bool(root_obj, "daemon", l_strlen("daemon"), &v ) ) {
		conf.daemon = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if ( OK == json_get_obj_num(root_obj, "worker_process", l_strlen("worker_process"), &v ) ) {
		conf.worker_process = (uint32)v->num;
		if( conf.worker_process > MAXPROCESS ) {
			err_log("%s --- work_procrss too much, [%d]", __func__, conf.worker_process );
			return ERROR;
		}
	}
	if( OK == json_get_obj_bool(root_obj, "reuse_port", l_strlen("reuse_port"), &v ) ) {
		conf.reuse_port = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if( OK == json_get_obj_bool(root_obj, "accept_mutex", l_strlen("accept_mutex"), &v ) ) {
		conf.accept_mutex = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if( OK == json_get_obj_str(root_obj, "sslcrt", l_strlen("sslcrt"), &v ) ) {
		conf.sslcrt.data = v->name.data;
		conf.sslcrt.len = v->name.len;
		memset( str, 0, sizeof(str) );
		l_memcpy( str, conf.sslcrt.data, conf.sslcrt.len );
		if( OK != access( str, F_OK ) ) {
			err_log("%s --- sslcrt file [%.*s] not found", __func__,
			conf.sslcrt.len, conf.sslcrt.data );
			conf.sslcrt.len = 0;
		}
	}
	if( OK == json_get_obj_str(root_obj, "sslkey", l_strlen("sslkey"), &v ) ) {
		conf.sslkey.data = v->name.data;
		conf.sslkey.len = v->name.len;
		memset( str, 0, sizeof(str) );
		l_memcpy( str, conf.sslkey.data, conf.sslkey.len );
		if( OK != access( str, F_OK ) ) {
			err_log("%s --- sslkey file [%.*s] not found", __func__,
		 	conf.sslkey.len, conf.sslkey.data );
			conf.sslkey.len = 0;
		}
	}
	if( OK == json_get_obj_bool(root_obj, "log_error", l_strlen("log_error"), &v ) ) {
		conf.log_error = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if( OK == json_get_obj_bool(root_obj, "log_debug", l_strlen("log_debug"), &v ) ) {
		conf.log_debug = (v->type == JSON_TRUE) ? 1 : 0;
	}
	return OK;
}
// config_parse_http -----------
static status config_parse_http( json_t * json )
{
	char str[1024] = {0};
	json_t * root_obj, *http_obj, *arr, *v;
	uint32 i;

	json_get_child( json, 1, &root_obj );
	if( OK ==  json_get_obj_obj(root_obj, "http", l_strlen("http"), &http_obj ) ) {
		if( OK == json_get_obj_bool(http_obj, "access_log", l_strlen("access_log"), &v ) ) {
			conf.http_access_log = (v->type == JSON_TRUE) ? 1 : 0;
		}
		if( OK == json_get_obj_bool(http_obj, "keepalive", l_strlen("keepalive"), &v ) ) {
			conf.http_keepalive = (v->type == JSON_TRUE) ? 1 : 0;
		}
		if( OK == json_get_obj_str(http_obj, "home", l_strlen("home"), &v ) ) {
			conf.home.data = v->name.data;
			conf.home.len = v->name.len;
			memset( str, 0, sizeof(str) );
			l_memcpy( str, conf.home.data, conf.home.len );
			if( OK != access( str, F_OK ) ) {
				err_log("%s --- home [%.*s] not found", __func__,
			 	conf.home.len, conf.home.data );
				conf.home.len = 0;
			}
		}
		if( OK == json_get_obj_str(http_obj, "index", l_strlen("index"), &v ) ) {
			conf.index.data = v->name.data;
			conf.index.len = v->name.len;
		}
		if( OK == json_get_obj_arr(http_obj, "http_listen", l_strlen("http_listen"), &arr ) ) {
			if( !conf.home.len ) {
				err_log("%s --- http need specify a valid 'home' and 'index'", __func__ );
				return ERROR;
			}
			for( i = 1; i <= arr->list->elem_num; i ++ ) {
				json_get_child( arr, i, &v );
				conf.http[conf.http_n++] = (uint32)v->num;
			}
		}
		if( OK == json_get_obj_arr(http_obj, "https_listen", l_strlen("https_listen"), &arr ) ) {
			if( !conf.home.len ) {
				err_log("%s --- https need specify a valid 'home' and 'index'", __func__ );
				return ERROR;
			}
			if( !conf.sslcrt.len || !conf.sslkey.len ) {
				err_log("%s --- https need specify a valid 'sslcrt' and 'sslkey'", __func__ );
				return ERROR;
			}
			for( i = 1; i <= arr->list->elem_num; i ++ ) {
				json_get_child( arr, i, &v );
				conf.https[conf.https_n++] = (uint32)v->num;
			}
		}
	}
	return OK;
}

// config_parse_perf -------------
static status config_parse_perf( json_t * json )
{
	json_t * root_obj, *perf_obj, *v;
	status rc;

	json_get_child( json, 1, &root_obj );

	if( OK == json_get_obj_obj(root_obj, "perf", l_strlen("perf"), &perf_obj ) ) {
		rc = json_get_obj_bool(perf_obj, "switch", l_strlen("switch"), &v );
		if( rc == ERROR ) {
			err_log("%s --- tunnel need specify a valid 'switch'", __func__ );
			return ERROR;
		}
		conf.perf_switch = (v->type == JSON_TRUE) ? 1 : 0;
	}
	return OK;
}
// config_parse -----
static status config_parse( json_t * json )
{
	if( OK != config_parse_global( json ) ) {
		err_log( "%s --- parse global", __func__ );
		return ERROR;
	}
	if( OK != config_parse_http( json ) ) {
		err_log("%s ---  parse http ", __func__ );
		return ERROR;
	}
	if( OK != config_parse_perf( json ) ) {
		err_log( "%s --- parse perf", __func__ );
		return ERROR;
	}
	return OK;
}
// config_start ----
static status config_start( void )
{
	json_t * json;
	meta_t * meta;

	if( OK != config_get( &meta, L_PATH_CONFIG ) ) {
		err_log( "%s --- configuration file open", __func__  );
		return ERROR;
	}
	debug_log( "%s --- configuration file:\n[%.*s]",
		__func__,
		meta_len( meta->pos, meta->last ),
		meta->pos
	);
	if( OK != json_decode( &json, meta->pos, meta->last ) ) {
		err_log("%s --- configuration file decode failed", __func__ );
		meta_free( meta );
		return ERROR;
	}
	conf.meta = meta;
	if( OK != config_parse( json ) ) {
		json_free( json );
		return ERROR;
	}
	json_free( json );
	return OK;
}
// config_init ----
status config_init ( void )
{
	memset( &conf, 0, sizeof(config_t) );
	conf.log_debug = 1;
	conf.log_error = 1;

	if( OK != config_start(  ) ) {
		return ERROR;
	}
	return OK;
}
// config_end -----
status config_end ( void )
{
	meta_free( conf.meta );
	return OK;
}

/*
 +----------------------------------------------------------------------+
 | PHP Version 5                                                        |
 +----------------------------------------------------------------------+
 | Copyright (c) 1997-2012 The PHP Group                                |
 +----------------------------------------------------------------------+
 | This source file is subject to version 3.01 of the PHP license,      |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.php.net/license/3_01.txt                                  |
 | If you did not receive a copy of the PHP license and are unable to   |
 | obtain it through the world-wide-web, please send a note to          |
 | license@php.net so we can mail you a copy immediately.               |
 +----------------------------------------------------------------------+
 | Author:                                                              |
 +----------------------------------------------------------------------+
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_swoole.h"
#include "php_main.h"
#include "swoole.h"
#include "Server.h"
#include "Client.h"

/**
 * PHP5.2
 */
#ifndef PHP_FE_END
#define PHP_FE_END {NULL,NULL,NULL}
#endif

#ifndef ZEND_MOD_END
#define ZEND_MOD_END {NULL,NULL,NULL}
#endif

#define SW_RES_SERVER_NAME          "SwooleServer"
#define SW_RES_CLIENT_NAME          "SwooleClient"
#define SW_MAX_FIND_COUNT             100 //最多一次性取100个connection_info
#define SW_PHP_CLIENT_BUFFER_SIZE     65535

#define PHP_SERVER_CALLBACK_NUM             10
//---------------------------------------------------
#define SW_SERVER_CB_onStart                0 //Server启动
#define SW_SERVER_CB_onConnect              1 //accept连接(Worker)
#define SW_SERVER_CB_onReceive              2 //接受数据
#define SW_SERVER_CB_onClose                3 //关闭连接(Worker)
#define SW_SERVER_CB_onShutdown             4 //Server关闭
#define SW_SERVER_CB_onTimer                5 //定时器
#define SW_SERVER_CB_onWorkerStart          6 //Worker进程启动
#define SW_SERVER_CB_onWorkerStop           7 //Worker进程结束
#define SW_SERVER_CB_onMasterConnect        8 //accept连接(master)
#define SW_SERVER_CB_onMasterClose          9 //关闭连接(master)

#define PHP_CLIENT_CALLBACK_NUM             3
//---------------------------------------------------
#define SW_CLIENT_CB_onConnect              0
#define SW_CLIENT_CB_onReceive              1
#define SW_CLIENT_CB_onClose                2
#define SW_CLIENT_CB_onError                3

#define SW_HOST_SIZE            128

static int le_swoole_server;
static int le_swoole_client;

#pragma pack(4)
typedef struct {
	uint16_t port;
	uint16_t from_fd;
} php_swoole_udp_t;
#pragma pack()

static zval *php_sw_callback[PHP_SERVER_CALLBACK_NUM];
static HashTable php_sw_reactor_callback;
static HashTable php_sw_client_callback;
#ifdef ZTS
static void ***sw_thread_ctx;
#endif
extern swReactor *swoole_worker_reactor;
static char php_sw_reactor_ok = 0;
static char php_sw_in_client = 0;
static int php_swoole_udp_from_fd = 0;
extern sapi_module_struct sapi_module;

static int php_swoole_onReceive(swFactory *, swEventData *);
static void php_swoole_onStart(swServer *);
static void php_swoole_onShutdown(swServer *);
static void php_swoole_onConnect(swServer *, int fd, int from_id);
static void php_swoole_onClose(swServer *, int fd, int from_id);
static void php_swoole_onTimer(swServer *serv, int interval);
static void php_swoole_onWorkerStart(swServer *, int worker_id);
static void php_swoole_onWorkerStop(swServer *, int worker_id);
static void php_swoole_onMasterConnect(swServer *, int fd, int from_id);
static void php_swoole_onMasterClose(swServer *, int fd, int from_id);
static void swoole_destory_server(zend_rsrc_list_entry *rsrc TSRMLS_DC);
static void swoole_destory_client(zend_rsrc_list_entry *rsrc TSRMLS_DC);

static int php_swoole_set_callback(int key, zval *cb TSRMLS_DC);
static int php_swoole_client_event_add(zval *sock_array, fd_set *fds, int *max_fd TSRMLS_DC);
static int php_swoole_client_event_loop(zval *sock_array, fd_set *fds TSRMLS_DC);
static int php_swoole_onReactorCallback(swReactor *reactor, swEvent *event);

static int php_swoole_client_onReceive(swReactor *reactor, swEvent *event);
static int php_swoole_client_onConnect(swReactor *reactor, swEvent *event);
static int php_swoole_client_onError(swReactor *reactor, swEvent *event);
static void php_swoole_check_reactor();
static void php_swoole_try_run_reactor();

#define SWOOLE_GET_SERVER(zobject, serv) zval **zserv;if (zend_hash_find(Z_OBJPROP_P(zobject), ZEND_STRS("_server"), (void **) &zserv) == FAILURE){ \
	php_error_docref(NULL TSRMLS_CC, E_WARNING, "Not have swoole server");\
	RETURN_FALSE;}\
	ZEND_FETCH_RESOURCE(serv, swServer *, zserv, -1, SW_RES_SERVER_NAME, le_swoole_server);


#ifdef SW_ASYNC_MYSQL
#include "ext/mysqlnd/mysqlnd.h"
#include "ext/mysqli/mysqli_mysqlnd.h"
#include "ext/mysqli/php_mysqli_structs.h"
#endif

const zend_function_entry swoole_functions[] =
{
	PHP_FE(swoole_version, NULL)
	PHP_FE(swoole_server_create, NULL)
	PHP_FE(swoole_server_set, NULL)
	PHP_FE(swoole_server_start, NULL)
	PHP_FE(swoole_server_send, NULL)
	PHP_FE(swoole_server_close, NULL)
	PHP_FE(swoole_server_handler, NULL)
	PHP_FE(swoole_server_addlisten, NULL)
	PHP_FE(swoole_server_addtimer, NULL)
	PHP_FE(swoole_server_addcontroller, NULL)
	PHP_FE(swoole_server_reload, NULL)
	PHP_FE(swoole_connection_info, NULL)
	PHP_FE(swoole_connection_list, NULL)
	PHP_FE(swoole_event_add, NULL)
	PHP_FE(swoole_event_del, NULL)
	PHP_FE(swoole_event_exit, NULL)
	PHP_FE(swoole_client_select, NULL)
	PHP_FE(swoole_set_process_name, NULL)
#ifdef SW_ASYNC_MYSQL
	PHP_FE(swoole_get_mysqli_sock, NULL)
#endif
	PHP_FE_END /* Must be the last line in swoole_functions[] */
};

static zend_function_entry swoole_server_methods[] = {
	PHP_FALIAS(__construct, swoole_server_create, NULL)
	PHP_FALIAS(set, swoole_server_set, NULL)
	PHP_FALIAS(start, swoole_server_start, NULL)
	PHP_FALIAS(send, swoole_server_send, NULL)
	PHP_FALIAS(close, swoole_server_close, NULL)
	PHP_FALIAS(addlistener, swoole_server_addlisten, NULL)
	PHP_FALIAS(addtimer, swoole_server_addtimer, NULL)
	PHP_FALIAS(reload, swoole_server_reload, NULL)
	PHP_FALIAS(handler, swoole_server_handler, NULL)
	PHP_FALIAS(connection_info, swoole_connection_info, NULL)
	PHP_FALIAS(connection_list, swoole_connection_list, NULL)
	{NULL, NULL, NULL}
};

const zend_function_entry swoole_client_methods[] =
{
	PHP_ME(swoole_client, __construct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
	PHP_ME(swoole_client, connect, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(swoole_client, recv, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(swoole_client, send, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(swoole_client, close, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(swoole_client, on, NULL, ZEND_ACC_PUBLIC)
	PHP_FE_END /* Must be the last line in swoole_client_methods[] */
};

static zend_class_entry swoole_client_ce;
static zend_class_entry *swoole_client_class_entry_ptr;

static zend_class_entry swoole_server_ce;
static zend_class_entry *swoole_server_class_entry_ptr;

zend_module_entry swoole_module_entry =
{
	STANDARD_MODULE_HEADER,
	"swoole",
	swoole_functions,
	PHP_MINIT(swoole),
	PHP_MSHUTDOWN(swoole),
	PHP_RINIT(swoole), //RINIT
	PHP_RSHUTDOWN(swoole), //RSHUTDOWN
	PHP_MINFO(swoole),
    SWOOLE_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_SWOOLE
ZEND_GET_MODULE(swoole)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
 PHP_INI_BEGIN()
 STD_PHP_INI_ENTRY("swoole.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_swoole_globals, swoole_globals)
 STD_PHP_INI_ENTRY("swoole.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_swoole_globals, swoole_globals)
 PHP_INI_END()
 */
/* }}} */

/* {{{ php_swoole_init_globals
 */
/* Uncomment this function if you have INI entries
 static void php_swoole_init_globals(zend_swoole_globals *swoole_globals)
 {
 swoole_globals->global_value = 0;
 swoole_globals->global_string = NULL;
 }
 */
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(swoole)
{
	le_swoole_server = zend_register_list_destructors_ex(swoole_destory_server, NULL, SW_RES_SERVER_NAME, module_number);
	le_swoole_client = zend_register_list_destructors_ex(swoole_destory_client, NULL, SW_RES_CLIENT_NAME, module_number);

	REGISTER_LONG_CONSTANT("SWOOLE_BASE", SW_MODE_SINGLE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_THREAD", SW_MODE_THREAD, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_PROCESS", SW_MODE_PROCESS, CONST_CS | CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("SWOOLE_SOCK_TCP", SW_SOCK_TCP, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_SOCK_TCP6", SW_SOCK_TCP6, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_SOCK_UDP", SW_SOCK_UDP, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_SOCK_UDP6", SW_SOCK_UDP6, CONST_CS | CONST_PERSISTENT);

	REGISTER_LONG_CONSTANT("SWOOLE_SOCK_SYNC", SW_SOCK_SYNC, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_SOCK_ASYNC", SW_SOCK_ASYNC, CONST_CS | CONST_PERSISTENT);
    REGISTER_STRINGL_CONSTANT("SWOOLE_VERSION", SWOOLE_VERSION, sizeof(SWOOLE_VERSION) - 1, CONST_PERSISTENT | CONST_CS);

	INIT_CLASS_ENTRY(swoole_client_ce, "swoole_client", swoole_client_methods);
	swoole_client_class_entry_ptr = zend_register_internal_class(&swoole_client_ce TSRMLS_CC);

	zend_declare_property_long(swoole_client_class_entry_ptr, SW_STRL("errCode")-1, 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_long(swoole_client_class_entry_ptr, SW_STRL("sock")-1, 0, ZEND_ACC_PUBLIC TSRMLS_CC);

	INIT_CLASS_ENTRY(swoole_server_ce, "swoole_server", swoole_server_methods);
	swoole_server_class_entry_ptr = zend_register_internal_class(&swoole_server_ce TSRMLS_CC);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(swoole)
{
	/* uncomment this line if you have INI entries
	 UNREGISTER_INI_ENTRIES();
	 */
	return SUCCESS;
}
/* }}} */


/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(swoole)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "swoole support", "enabled");
	php_info_print_table_row(2, "Version", SWOOLE_VERSION);
	php_info_print_table_row(2, "Author", "tianfeng.han[email: mikan.tenny@gmail.com]");

#ifdef HAVE_EPOLL
	php_info_print_table_row(2, "epoll", "enable");
#endif
#ifdef HAVE_EVENTFD
    php_info_print_table_row(2, "event_fd", "enable");
#endif
#ifdef HAVE_KQUEUE
    php_info_print_table_row(2, "kqueue", "enable");
#endif
#ifdef HAVE_TIMERFD
    php_info_print_table_row(2, "timerfd", "enable");
#endif
#ifdef SW_USE_ACCEPT4
    php_info_print_table_row(2, "accept4", "enable");
#endif
#ifdef HAVE_CPU_AFFINITY
    php_info_print_table_row(2, "cpu affinity", "enable");
#endif
	php_info_print_table_end();
}
/* }}} */

PHP_RINIT_FUNCTION(swoole)
{
	//swoole_event_add
	zend_hash_init(&php_sw_reactor_callback, 16, NULL, ZVAL_PTR_DTOR, 0);
	//swoole_client::on
	zend_hash_init(&php_sw_client_callback, 16, NULL, ZVAL_PTR_DTOR, 0);

	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(swoole)
{
	zend_hash_destroy(&php_sw_reactor_callback);
	zend_hash_destroy(&php_sw_client_callback);
	return SUCCESS;
}

static void swoole_destory_server(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	swoole_running = 0;
	swServer *serv = (swServer *) rsrc->ptr;
	if(serv!=NULL)
	{
		//只有主进程执行此操作
		if(swIsMaster())
		{
			swServer_free(serv);
		}
		sw_free(serv);
	}
}

static void swoole_destory_client(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	swClient *cli = (swClient *) rsrc->ptr;
	if(cli->sock != 0)
	{
		cli->close(cli);
	}
	efree(cli);
}

PHP_FUNCTION(swoole_version)
{
    php_printf("swoole v%s\n", SWOOLE_VERSION);
}


#ifdef SW_ASYNC_MYSQL
PHP_FUNCTION(swoole_get_mysqli_sock)
{
	MY_MYSQL *mysql;
	zval *mysql_link;
	int sock;
	extern zend_class_entry *mysqli_link_class_entry;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &mysql_link) == FAILURE)
	{
		return;
	}
	MYSQLI_FETCH_RESOURCE_CONN(mysql, &mysql_link, MYSQLI_STATUS_VALID);
	if (SUCCESS != php_stream_cast(mysql->mysql->data->net->stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL,
											(void*)&sock, 1) && sock >= 0)
	{
		RETURN_FALSE;
	}
	else
	{
		RETURN_LONG(sock);
	}
}
#endif

PHP_FUNCTION(swoole_server_create)
{
	int host_len;
	char *serv_host;
	long sock_type = SW_SOCK_TCP;
	long serv_port;
	long serv_mode = SW_MODE_PROCESS;

	//only cli env
	if(strcasecmp("cli", sapi_module.name) != 0)
	{
		zend_error(E_ERROR, "SwooleServer must run at php_cli environment.");
		RETURN_FALSE;
	}
	swServer *serv = sw_malloc(sizeof(swServer));
	swServer_init(serv);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl|ll", &serv_host, &host_len, &serv_port, &serv_mode, &sock_type) == FAILURE)
	{
		return;
	}

	if(serv_mode == SW_MODE_THREAD)
	{
		serv_mode = SW_MODE_SINGLE;
		swWarn("PHP can not running at multi-threading. Reset mode to SW_MODE_BASE");
	}

	serv->factory_mode = serv_mode;
	swTrace("Create host=%s,port=%ld,mode=%d\n", serv_host, serv_port, serv->factory_mode);

	//线程安全
	TSRMLS_SET_CTX(sw_thread_ctx);

	bzero(php_sw_callback, sizeof(zval*)*PHP_SERVER_CALLBACK_NUM);

	if(swServer_addListen(serv, sock_type, serv_host, serv_port) < 0)
	{
		zend_error(E_ERROR, "swServer_addListen fail. Error: %s [%d]", strerror(errno), errno);
	}
	if (!getThis())
	{
		object_init_ex(return_value, swoole_server_class_entry_ptr);
		getThis() = return_value;
	}
	zval *zres;
	MAKE_STD_ZVAL(zres);
	ZEND_REGISTER_RESOURCE(zres, serv, le_swoole_server);
	zend_update_property(swoole_server_class_entry_ptr, getThis(), ZEND_STRL("_server"), zres TSRMLS_CC);
}

PHP_FUNCTION(swoole_server_set)
{
	zval *zset = NULL;
	zval *zobject = getThis();
	HashTable * vht;
	swServer *serv;
	zval **v;
	double timeout;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oa", &zobject, swoole_server_class_entry_ptr, &zset) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &zset) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);

	vht = Z_ARRVAL_P(zset);
	//timeout
	if (zend_hash_find(vht, ZEND_STRS("timeout"), (void **)&v) == SUCCESS)
	{
		convert_to_double(*v);
		timeout = Z_DVAL_PP(v);
		serv->timeout_sec = (int)timeout;
		serv->timeout_usec = (int)((timeout*1000*1000) - (serv->timeout_sec*1000*1000));
	}
	//daemonize，守护进程化
	if (zend_hash_find(vht, ZEND_STRS("daemonize"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->daemonize = (int)Z_LVAL_PP(v);
	}
	//backlog
	if (zend_hash_find(vht, ZEND_STRS("backlog"), (void **)&v) == SUCCESS)
	{
		serv->backlog = (int)Z_LVAL_PP(v);
	}
	//poll_thread_num
	if (zend_hash_find(vht, ZEND_STRS("poll_thread_num"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->poll_thread_num = (int)Z_LVAL_PP(v);
	}
	//writer_num
	if (zend_hash_find(vht, ZEND_STRS("writer_num"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->writer_num = (int)Z_LVAL_PP(v);
	}
	//writer_num
	if (zend_hash_find(vht, ZEND_STRS("worker_num"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->worker_num = (int)Z_LVAL_PP(v);
	}
	//max_conn
	if (zend_hash_find(vht, ZEND_STRS("max_conn"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->max_conn = (int)Z_LVAL_PP(v);
	}
	//max_request
	if (zend_hash_find(vht, ZEND_STRS("max_request"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->max_request = (int)Z_LVAL_PP(v);
	}
	//cpu affinity
	if (zend_hash_find(vht, ZEND_STRS("open_cpu_affinity"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->open_cpu_affinity = (uint8_t)Z_LVAL_PP(v);
	}
	//tcp_nodelay
	if (zend_hash_find(vht, ZEND_STRS("open_tcp_nodelay"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->open_tcp_nodelay = (uint8_t)Z_LVAL_PP(v);
	}
	//tcp_keepalive
	if (zend_hash_find(vht, ZEND_STRS("open_tcp_keepalive"), (void **)&v) == SUCCESS)
	{
		serv->open_tcp_keepalive = (uint8_t)Z_LVAL_PP(v);
	}
	//data buffer, EOF检测
	if (zend_hash_find(vht, ZEND_STRS("open_eof_check"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->open_eof_check = (uint8_t)Z_LVAL_PP(v);
	}
	//data eof设置
	if (zend_hash_find(vht, ZEND_STRS("data_eof"), (void **) &v) == SUCCESS)
	{
		if (Z_STRLEN_PP(v) > SW_DATA_EOF_MAXLEN)
		{
			zend_error(E_ERROR, "swoole_server date_eof max length is %d", SW_DATA_EOF_MAXLEN);
			RETURN_FALSE;
		}
		memcpy(serv->data_eof, Z_STRVAL_PP(v), Z_STRLEN_PP(v));
	}
	//tcp_keepidle
	if (zend_hash_find(vht, ZEND_STRS("tcp_keepidle"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->tcp_keepidle = (uint16_t)Z_LVAL_PP(v);
	}
	//tcp_keepinterval
	if (zend_hash_find(vht, ZEND_STRS("tcp_keepinterval"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->tcp_keepinterval = (uint16_t)Z_LVAL_PP(v);
	}
	//tcp_keepcount
	if (zend_hash_find(vht, ZEND_STRS("tcp_keepcount"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->tcp_keepcount = (uint16_t)Z_LVAL_PP(v);
	}
	//max_request
	if (zend_hash_find(vht, ZEND_STRS("dispatch_mode"), (void **)&v) == SUCCESS)
	{
		convert_to_long(*v);
		serv->dispatch_mode = (int)Z_LVAL_PP(v);
	}
	//log_file
	if (zend_hash_find(vht, ZEND_STRS("log_file"), (void **)&v) == SUCCESS)
	{
		memcpy(serv->log_file, Z_STRVAL_PP(v), Z_STRLEN_PP(v));
	}
	RETURN_TRUE;
}

static int php_swoole_set_callback(int key, zval *cb TSRMLS_DC)
{
	char *func_name = NULL;
	if(!zend_is_callable(cb, 0, &func_name TSRMLS_CC))
	{
		zend_error(E_WARNING, "Function '%s' is not callable", func_name);
		efree(func_name);
		return SW_ERR;
	}
	//zval_add_ref(&cb);
	php_sw_callback[key] = pemalloc(sizeof(zval), 1);
	if(php_sw_callback[key] == NULL)
	{
		return SW_ERR;
	}
	*(php_sw_callback[key]) = *cb;
	zval_copy_ctor(php_sw_callback[key]);
	return SW_OK;
}

PHP_FUNCTION(swoole_server_handler)
{
	zval *zobject = getThis();
	char *ha_name = NULL;
	int len, i;
	int ret = -1;
	swServer *serv;
	zval *cb;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Osz", &zobject, swoole_server_class_entry_ptr, &ha_name, &len, &cb) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz", &ha_name, &len, &cb) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);

	//必须与define顺序一致
	char *callback[PHP_SERVER_CALLBACK_NUM] = {
			"onStart",
			"onConnect",
			"onReceive",
			"onClose",
			"onShutdown",
			"onTimer",
			"onWorkerStart",
			"onWorkerStop",
			"onMasterConnect",
			"onMasterClose",
	};
	for(i=0; i<PHP_SERVER_CALLBACK_NUM; i++)
	{
		if(strncasecmp(callback[i], ha_name, len) == 0)
		{
			ret = php_swoole_set_callback(i, cb TSRMLS_CC);
			break;
		}
	}
	if(ret < 0)
	{
		zend_error(E_ERROR, "swoole_server_handler: unkown handler[%s].", ha_name);
	}
	ZVAL_BOOL(return_value, ret);
}

PHP_FUNCTION(swoole_server_close)
{
	zval *zobject = getThis();;
	swServer *serv;
	swEvent ev;
	long conn_fd, from_id = -1;
	int ret;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ol|l", &zobject, swoole_server_class_entry_ptr, &conn_fd, &from_id) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &conn_fd, &from_id) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);

	if(from_id < 0)
	{
		ev.from_id = serv->factory.last_from_id;
	}
	else
	{
		ev.from_id = from_id;
	}
	ev.fd = (int)conn_fd;
	//主进程不应当执行此操作
	if(swIsMaster())
	{
		RETURN_FALSE;
	}
	SW_CHECK_RETURN(serv->factory.end(&serv->factory, &ev));
}

PHP_FUNCTION(swoole_server_reload)
{
	zval *zobject = getThis();;
	swServer *serv;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zobject) == FAILURE)
	{
		return;
	}

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &zobject, swoole_server_class_entry_ptr) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);
	SW_CHECK_RETURN(swServer_reload(serv));
}

PHP_FUNCTION(swoole_connection_info)
{
	zval *zobject = getThis();
	swServer *serv;
	long fd = 0;
	long from_id = 0;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ol|l", &zobject, swoole_server_class_entry_ptr, &fd, &from_id) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &fd) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);

	swConnection *conn = swServer_get_connection(serv, fd);
	//It's udp
	if(conn == NULL || php_swoole_udp_from_fd != 0)
	{
		array_init(return_value);
		swConnection *from_sock = swServer_get_connection(serv, php_swoole_udp_from_fd);
		struct in_addr sin_addr;
		sin_addr.s_addr = fd;
		if (from_sock != NULL)
		{
			add_assoc_long(return_value, "from_fd", php_swoole_udp_from_fd);
			add_assoc_long(return_value, "from_port",  serv->connection_list[php_swoole_udp_from_fd].addr.sin_port);
		}
		if (from_id !=0 )
		{
			add_assoc_long(return_value, "remote_port", ntohs(from_id));
		}
		add_assoc_string(return_value, "remote_ip", inet_ntoa(sin_addr), 1);
		return;
	}
	if(conn->tag == 0)
	{
		RETURN_FALSE;
	}
	else
	{
		array_init(return_value);
		add_assoc_long(return_value, "from_id", conn->from_id);
		add_assoc_long(return_value, "from_fd", conn->from_fd);
		add_assoc_long(return_value, "from_port",  serv->connection_list[conn->from_fd].addr.sin_port);
		add_assoc_long(return_value, "remote_port", ntohs(conn->addr.sin_port));
		add_assoc_string(return_value, "remote_ip", inet_ntoa(conn->addr.sin_addr), 1);
	}
}

PHP_FUNCTION(swoole_connection_list)
{
	zval *zobject = getThis();
	swServer *serv;
	long start_fd = 0;
	long find_count = 10;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|ll", &zobject, swoole_server_class_entry_ptr, &start_fd, &find_count) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|ll", &start_fd, &find_count) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);


	//超过最大查找数量
	if (find_count > SW_MAX_FIND_COUNT)
	{
		zend_error(E_WARNING, "swoole_connection_list max_find_count=%d", SW_MAX_FIND_COUNT);
		RETURN_FALSE;
	}

	//复制出来避免被其他进程改写
	int serv_max_fd = swServer_get_maxfd(serv);

	if(start_fd == 0)
	{
		start_fd = swServer_get_minfd(serv);
	}

	//达到最大，表示已经取完了
	if ((int)start_fd >= serv_max_fd)
	{
		RETURN_FALSE;
	}
	array_init(return_value);
	int fd = start_fd+1;

	//循环到最大fd
	for(; fd<= serv_max_fd; fd++)
	{
		 swTrace("maxfd=%d|fd=%d|find_count=%d|start_fd=%d", serv_max_fd, fd, find_count, start_fd);
		 if(serv->connection_list[fd].tag == 1)
		 {
			 add_next_index_long(return_value, fd);
			 find_count--;
		 }
		 //finish fetch
		 if(find_count <= 0)
		 {
			 break;
		 }
	}
	//sw_log(SW_END_LINE);
}

int php_swoole_onReceive(swFactory *factory, swEventData *req)
{
	swServer *serv = factory->ptr;
	zval *zserv = (zval *)serv->ptr2;
	zval **args[4];

	zval *zfd;
	zval *zfrom_id;
	zval *zdata;
	zval *retval;

	int from_id;

	MAKE_STD_ZVAL(zfd);
	ZVAL_LONG(zfd, (long)req->info.fd);

	MAKE_STD_ZVAL(zfrom_id);
	if(req->info.from_id >= serv->poll_thread_num)
	{
		//UDP使用from_id作为port,fd做为ip
		php_swoole_udp_t udp_info;
		php_swoole_udp_from_fd = udp_info.from_fd = req->info.from_fd;
		udp_info.port = req->info.from_id;
		memcpy(&from_id, &udp_info, sizeof(from_id));
		swTrace("SendTo: from_id=%d|from_fd=%d", req->info.from_fd, req->info.from_id);
		ZVAL_LONG(zfrom_id, (long) from_id);
	}
	else
	{
		php_swoole_udp_from_fd = 0;
		ZVAL_LONG(zfrom_id, (long)req->info.from_id);
	}

	MAKE_STD_ZVAL(zdata);
	ZVAL_STRINGL(zdata, req->data, req->info.len, 1);

	args[0] = &zserv;
	args[1] = &zfd;
	args[2] = &zfrom_id;
	args[3] = &zdata;

	//printf("req: fd=%d|len=%d|from_id=%d|data=%s\n", req->fd, req->len, req->from_id, req->data);

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onReceive], &retval, 4, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwoolServer: onReceive handler error");
	}
	zval_ptr_dtor(&zfd);
	zval_ptr_dtor(&zfrom_id);
	zval_ptr_dtor(&zdata);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
	return SW_OK;
}

void php_swoole_onStart(swServer *serv)
{
	zval *zserv = (zval *)serv->ptr2;
	zval **args[1];
	zval *retval;

	zval *zmaster_pid, *zmanager_pid;

	MAKE_STD_ZVAL(zmaster_pid);
	ZVAL_LONG(zmaster_pid, getpid());

	MAKE_STD_ZVAL(zmanager_pid);
	ZVAL_LONG(zmanager_pid, (serv->factory_mode == SW_MODE_PROCESS)?swServer_get_manager_pid(serv):0);

	zend_update_property(swoole_server_class_entry_ptr, zserv, ZEND_STRL("master_pid"), zmaster_pid TSRMLS_CC);
	zend_update_property(swoole_server_class_entry_ptr, zserv, ZEND_STRL("manager_pid"), zmanager_pid TSRMLS_CC);

	args[0] = &zserv;
	zval_add_ref(&zserv);
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onStart], &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onStart handler error");
	}
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

void php_swoole_onTimer(swServer *serv, int interval)
{
	zval *zserv = (zval *)serv->ptr2;
	zval **args[2];
	zval *retval;
	zval *zinterval;

	MAKE_STD_ZVAL(zinterval);
	ZVAL_LONG(zinterval, interval);

	args[0] = &zserv;
	args[1] = &zinterval;
	zval_add_ref(&zserv);
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onTimer], &retval, 2, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onTimer handler error");
	}
	zval_ptr_dtor(&zinterval);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

void php_swoole_onShutdown(swServer *serv)
{
	zval *zserv = (zval *)serv->ptr2;
	zval **args[1];
	zval *retval;

	args[0] = &zserv;
	zval_add_ref(&zserv);
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onShutdown], &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onShutdown handler error");
	}
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

static void php_swoole_onWorkerStart(swServer *serv, int worker_id)
{
	zval *zserv = (zval *)serv->ptr2;
	zval *zworker_id;
	zval **args[2]; //这里必须与下面的数字对应
	zval *retval;

	MAKE_STD_ZVAL(zworker_id);
	ZVAL_LONG(zworker_id, worker_id);

	args[0] = &zserv;
	zval_add_ref(&zserv);
	args[1] = &zworker_id;

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onWorkerStart], &retval, 2, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onShutdown handler error");
	}
	zval_ptr_dtor(&zworker_id);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

static void php_swoole_onWorkerStop(swServer *serv, int worker_id)
{
	zval *zobject = (zval *)serv->ptr2;
	zval *zworker_id;
	zval **args[2]; //这里必须与下面的数字对应
	zval *retval;

	MAKE_STD_ZVAL(zworker_id);
	ZVAL_LONG(zworker_id, worker_id);

	zval_add_ref(&zobject);
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if (php_sw_callback[SW_SERVER_CB_onWorkerStop] == NULL)
	{
		args[0] = &zworker_id;
		zval func;
		ZVAL_STRING(&func, "onWorkerStop", 0);

		if (call_user_function_ex(EG(function_table), &zobject, &func, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
		{
			zend_error(E_WARNING, "SwooleServer->onShutdown handler error");
		}
	}
	else
	{
		args[0] = &zobject;
		args[1] = &zworker_id;
		if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onWorkerStop], &retval, 2, args, 0, NULL TSRMLS_CC) == FAILURE)
		{
			zend_error(E_WARNING, "SwooleServer: onShutdown handler error");
		}
	}

	zval_ptr_dtor(&zworker_id);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

static int php_swoole_onReactorCallback(swReactor *reactor, swEvent *event)
{
	zval *zfd;
	zval *zfrom_id;
	zval **args[2];
	zval *retval;
	zval *callback;

	MAKE_STD_ZVAL(zfd);
	ZVAL_LONG(zfd, event->fd);

	MAKE_STD_ZVAL(zfrom_id);
	ZVAL_LONG(zfrom_id, event->from_id);

	args[0] = &zfd;
	args[1] = &zfrom_id;

	if(zend_hash_find(&php_sw_reactor_callback, (char *)&(event->fd), sizeof(event->fd), &callback) != SUCCESS)
	{
		zend_error(E_WARNING, "SwooleServer: onReactorCallback not found");
		return SW_ERR;
	}

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function_ex(EG(function_table), NULL, callback, &retval, 2, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onReactorCallback handler error");
		return SW_ERR;
	}
	zval_ptr_dtor(&zfrom_id);
	zval_ptr_dtor(&zfd);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
	return SW_OK;
}

static int php_swoole_client_onReceive(swReactor *reactor, swEvent *event)
{
	zval **zobject, *zcallback = NULL;
	zval **args[1];
	zval *retval;

	char *hash_key;
	int hash_key_len;
	hash_key_len = spprintf(&hash_key, 0, "%d", event->fd);

	if(zend_hash_find(&php_sw_client_callback, hash_key, hash_key_len+1, &zobject) != SUCCESS)
	{
		zend_error(E_WARNING, "swoole_client: Fd[%d] is not a swoole_client object", event->fd);
		return SW_ERR;
	}

	args[0] = zobject;
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	zcallback = zend_read_property(swoole_client_class_entry_ptr, *zobject, SW_STRL("receive")-1, 0 TSRMLS_CC);
	if (zcallback == NULL)
	{
		zend_error(E_WARNING, "SwooleClient: swoole_client object have not receive callback.");
		return SW_ERR;
	}
	if (call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onReactorCallback handler error");
		return SW_ERR;
	}
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
	return SW_OK;
}

static int php_swoole_client_onConnect(swReactor *reactor, swEvent *event)
{
	zval **zobject = NULL, *zcallback = NULL;
	zval **args[1];
	zval *retval;

	char *hash_key;
	int hash_key_len;
	hash_key_len = spprintf(&hash_key, 0, "%d", event->fd);

	if(zend_hash_find(&php_sw_client_callback, hash_key, hash_key_len+1, &zobject) != SUCCESS)
	{
		zend_error(E_WARNING, "swoole_client->onConnect: Fd=%d is not a swoole_client object", event->fd);
		return SW_ERR;
	}
	args[0] = zobject;
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	int error, len = sizeof(error);
	if (getsockopt (event->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
	{
		zend_error(E_WARNING, "swoole_client: getsockopt[sock=%d] fail.Error: %s [%d]", event->fd, strerror(errno), errno);
		return SW_ERR;
	}
	//success
	if(error == 0)
	{
		swoole_worker_reactor->set(swoole_worker_reactor, event->fd, (SW_FD_USER+1) | SW_EVENT_READ | SW_EVENT_ERROR);
		zcallback = zend_read_property(swoole_client_class_entry_ptr, *zobject, SW_STRL("connect")-1, 0 TSRMLS_CC);
		if (zcallback == NULL)
		{
			zend_error(E_WARNING, "SwooleClient: swoole_client object have not connect callback.");
			return SW_ERR;
		}
		if (call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
		{
			zend_error(E_WARNING, "SwooleServer: onReactorCallback handler error");
			return SW_ERR;
		}
	}
	else
	{
		zend_error(E_WARNING, "SwooleClient: connect to server fail. Error: %s [%d]", strerror(errno), errno);
		swoole_worker_reactor->del(swoole_worker_reactor, event->fd);
		zcallback = zend_read_property(swoole_client_class_entry_ptr, *zobject, SW_STRL("error")-1, 0 TSRMLS_CC);
		if (zcallback == NULL)
		{
			zend_error(E_WARNING, "SwooleClient: swoole_client object have not error callback.");
			return SW_ERR;
		}
		if (call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
		{
			zend_error(E_WARNING, "SwooleServer: onReactorCallback handler error");
			return SW_ERR;
		}
	}
	return SW_OK;
}

static int php_swoole_client_onError(swReactor *reactor, swEvent *event)
{
	zval **zobject, *zcallback = NULL;
	zval **args[1];
	zval *retval;

	char *hash_key;
	int hash_key_len;
	hash_key_len = spprintf(&hash_key, 0, "%d", event->fd);

	if (zend_hash_find(&php_sw_client_callback, hash_key, hash_key_len + 1, &zobject) != SUCCESS)
	{
		zend_error(E_WARNING, "swoole_client: Fd[%d] is not a swoole_client object", event->fd);
		return SW_ERR;
	}

	args[0] = zobject;
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	zcallback = zend_read_property(swoole_client_class_entry_ptr, *zobject, SW_STRL("error")-1, 0 TSRMLS_CC);
	if (zcallback == NULL)
	{
		zend_error(E_WARNING, "SwooleClient: swoole_client object have not error callback.");
		return SW_ERR;
	}
	if (call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onReactorCallback handler error");
		return SW_ERR;
	}
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
	return SW_OK;
}

static void php_swoole_check_reactor()
{
	if(php_sw_reactor_ok == 0)
	{
		if (swoole_worker_reactor == NULL)
		{
			swoole_worker_reactor = sw_malloc(sizeof(swReactor));
			if(swoole_worker_reactor == NULL)
			{
				zend_error(E_ERROR, "swoole_client: malloc swoole_worker_reactor fail");
				return;
			}
			if(swReactorSelect_create(swoole_worker_reactor) < 0)
			{
				zend_error(E_ERROR, "swoole_client: create swoole_worker_reactor fail");
				return;
			}
			//client, swoole_event_exit will set swoole_running = 0
			php_sw_in_client = 1;
		}
		swoole_worker_reactor->setHandle(swoole_worker_reactor, SW_FD_WRITE, php_swoole_client_onConnect);
		swoole_worker_reactor->setHandle(swoole_worker_reactor, SW_FD_ERROR, php_swoole_client_onError);

		swoole_worker_reactor->setHandle(swoole_worker_reactor, SW_FD_USER, php_swoole_onReactorCallback);
		swoole_worker_reactor->setHandle(swoole_worker_reactor, SW_FD_USER+1, php_swoole_client_onReceive);
		php_sw_reactor_ok = 1;
	}
	return;
}

static void php_swoole_try_run_reactor()
{
	if (swoole_running == 0)
	{
		swoole_running = 1;
		struct timeval timeo;
		timeo.tv_sec = SW_REACTOR_TIMEO_SEC;
		timeo.tv_usec = SW_REACTOR_TIMEO_USEC;

		int ret = swoole_worker_reactor->wait(swoole_worker_reactor, &timeo);
		if(ret < 0)
		{
			zend_error(E_ERROR, "swoole_client: reactor wait fail. Errno: %s [%d]", strerror(errno), errno);
		}
	}
}

void php_swoole_onConnect(swServer *serv, int fd, int from_id)
{
	zval *zserv = (zval *) serv->ptr2;
	zval *zfd;
	zval *zfrom_id;
	zval **args[3];
	zval *retval;

	MAKE_STD_ZVAL(zfd);
	ZVAL_LONG(zfd, fd);

	MAKE_STD_ZVAL(zfrom_id);
	ZVAL_LONG(zfrom_id, from_id);

	args[0] = &zserv;
	zval_add_ref(&zserv);
	args[1] = &zfd;
	args[2] = &zfrom_id;

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onConnect], &retval, 3, args, 0,
			NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onConnect handler error");
	}
	zval_ptr_dtor(&zfd);
	zval_ptr_dtor(&zfrom_id);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

void php_swoole_onClose(swServer *serv, int fd, int from_id)
{
	zval *zserv = (zval *) serv->ptr2;
	zval *zfd;
	zval *zfrom_id;
	zval **args[3];
	zval *retval;

	MAKE_STD_ZVAL(zfd);
	ZVAL_LONG(zfd, fd);

	MAKE_STD_ZVAL(zfrom_id);
	ZVAL_LONG(zfrom_id, from_id);

	args[0] = &zserv;
	zval_add_ref(&zserv);
	args[1] = &zfd;
	args[2] = &zfrom_id;

//	php_printf("fd=%d|from_id=%d\n", fd, from_id);

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onClose], &retval, 3, args, 0,
			NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onClose handler error");
	}
	zval_ptr_dtor(&zfd);
	zval_ptr_dtor(&zfrom_id);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

void php_swoole_onMasterConnect(swServer *serv, int fd, int from_id)
{
	zval *zserv = (zval *) serv->ptr2;
	zval *zfd;
	zval *zfrom_id;
	zval **args[3];
	zval *retval;

	MAKE_STD_ZVAL(zfd);
	ZVAL_LONG(zfd, fd);

	MAKE_STD_ZVAL(zfrom_id);
	ZVAL_LONG(zfrom_id, from_id);

	args[0] = &zserv;
	zval_add_ref(&zserv);
	args[1] = &zfd;
	args[2] = &zfrom_id;

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onMasterConnect], &retval, 3, args, 0,
			NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: ononMasterConnect handler error");
	}
	zval_ptr_dtor(&zfd);
	zval_ptr_dtor(&zfrom_id);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

void php_swoole_onMasterClose(swServer *serv, int fd, int from_id)
{
	zval *zserv = (zval *) serv->ptr2;
	zval *zfd;
	zval *zfrom_id;
	zval **args[3];
	zval *retval;

	MAKE_STD_ZVAL(zfd);
	ZVAL_LONG(zfd, fd);

	MAKE_STD_ZVAL(zfrom_id);
	ZVAL_LONG(zfrom_id, from_id);

	args[0] = &zserv;
	zval_add_ref(&zserv);
	args[1] = &zfd;
	args[2] = &zfrom_id;

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function_ex(EG(function_table), NULL, php_sw_callback[SW_SERVER_CB_onMasterClose], &retval, 3, args, 0,
			NULL TSRMLS_CC) == FAILURE)
	{
		zend_error(E_WARNING, "SwooleServer: onMasterClose handler error");
	}
	zval_ptr_dtor(&zfd);
	zval_ptr_dtor(&zfrom_id);
	if (retval != NULL)
	{
		zval_ptr_dtor(&retval);
	}
}

PHP_FUNCTION(swoole_server_start)
{
	zval *zobject = getThis();
	swServer *serv;
	int ret;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &zobject, swoole_server_class_entry_ptr) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);

	if(php_sw_callback[SW_SERVER_CB_onStart]!=NULL)
	{
		serv->onStart = php_swoole_onStart;
	}
	if(php_sw_callback[SW_SERVER_CB_onShutdown]!=NULL)
	{
		serv->onShutdown = php_swoole_onShutdown;
	}
	if(php_sw_callback[SW_SERVER_CB_onMasterConnect]!=NULL)
	{
		serv->onMasterConnect = php_swoole_onMasterConnect;
	}
	if(php_sw_callback[SW_SERVER_CB_onMasterClose]!=NULL)
	{
		serv->onMasterClose = php_swoole_onMasterClose;
	}
	if(php_sw_callback[SW_SERVER_CB_onWorkerStart]!=NULL)
	{
		serv->onWorkerStart = php_swoole_onWorkerStart;
	}
	if(php_sw_callback[SW_SERVER_CB_onWorkerStop]!=NULL)
	{
		serv->onWorkerStop = php_swoole_onWorkerStop;
	}
	if(php_sw_callback[SW_SERVER_CB_onTimer]!=NULL)
	{
		serv->onTimer = php_swoole_onTimer;
	}

	serv->onClose = php_swoole_onClose;
	serv->onConnect = php_swoole_onConnect;
	serv->onReceive = php_swoole_onReceive;

	zval_add_ref(&zobject);
	serv->ptr2 = zobject;
	ret = swServer_create(serv);

	if (ret < 0)
	{
		zend_error(E_ERROR, "SwooleServer: create server fail. Error: %s [%d][sw_error=%s]", strerror(errno), errno, sw_error);
		RETURN_LONG(ret);
	}
	ret = swServer_start(serv);
	if (ret < 0)
	{
		zend_error(E_ERROR, "SwooleServer: start server fail. Error: %s [%d][sw_error=%s]", strerror(errno), errno, sw_error);
		RETURN_LONG(ret);
	}
	RETURN_TRUE;
}

PHP_FUNCTION(swoole_server_send)
{
	zval *zobject = getThis();
	swServer *serv = NULL;
	swFactory *factory = NULL;
	swSendData _send;
	char buffer[SW_BUFFER_SIZE];

	char *send_data;
	int send_len;

	long conn_fd;
	long from_id = -1;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ols|l", &zobject, swoole_server_class_entry_ptr, &conn_fd, &send_data,
				&send_len, &from_id) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ls|l", &conn_fd, &send_data,
				&send_len, &from_id) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);


	factory = &(serv->factory);

	_send.info.fd = (int)conn_fd;
	if (from_id < 0)
	{
		_send.info.from_id = factory->last_from_id;
	}
	//TCP
	else if((uint32_t)from_id < serv->poll_thread_num)
	{
		_send.info.from_id = from_id;
	}
	//UDP
	else
	{
		php_swoole_udp_t udp_info;
		memcpy(&udp_info, (uint32_t *)(&from_id), sizeof(udp_info));
		_send.info.from_id = (uint16_t)(udp_info.port);
		_send.info.from_fd = (uint16_t)(udp_info.from_fd);
//		swWarn("SendTo: from_id=%d|from_fd=%d", _send.info.from_id, _send.info.from_fd);
	}
	_send.data = buffer;

	int ret, i;

	//分页发送，需要去掉头部所在的尺寸
	int pagesize = SW_BUFFER_SIZE - sizeof(_send.info);

	int trunk_num = (send_len/pagesize) + 1;
	int send_n = 0;
//	swWarn("SendTo: trunk_num=%d|send_len=%d", trunk_num, send_len);
	for(i=0; i<trunk_num; i++)
	{
		//最后一页
		if(i == (trunk_num-1))
		{
			send_n = send_len % pagesize;
			if(send_n == 0) break;
		}
		else
		{
			send_n = pagesize;
		}
		memcpy(buffer, send_data + pagesize*i, send_n);
		_send.info.len = send_n;
		ret = factory->finish(factory, &_send);
	}
	SW_CHECK_RETURN(ret);
}

PHP_FUNCTION(swoole_server_addlisten)
{
	zval *zobject = getThis();
	swServer *serv = NULL;
	swFactory *factory = NULL;
	char *host;
	int host_len;
	long sock_type;
	long port;
	int ret;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Osll", &zobject, swoole_server_class_entry_ptr, &host, &host_len, &port, &sock_type) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sll", &host, &host_len, &port, &sock_type) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);
	SW_CHECK_RETURN(swServer_addListen(serv, (int)sock_type, host, (int)port));
}

PHP_FUNCTION(swoole_event_add)
{
	zval *cb;
	long fd;
	int _fd;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lz", &fd, &cb) == FAILURE)
	{
		return;
	}
	_fd = (int)fd;
	zval *value = pemalloc(sizeof(zval), 1);
	*(value) = *cb;
	zval_copy_ctor(value);
	if(zend_hash_update(&php_sw_reactor_callback, &_fd, sizeof(_fd), value, sizeof(zval), NULL) == FAILURE)
	{
		zend_error(E_WARNING, "swoole_event_add add to hashtable fail");
		RETURN_FALSE;
	}
	php_swoole_check_reactor();
	swSetNonBlock(_fd); //must be nonblock
	int ret = swoole_worker_reactor->add(swoole_worker_reactor, _fd, SW_FD_USER);
	php_swoole_try_run_reactor();
	SW_CHECK_RETURN(ret);
}

PHP_FUNCTION(swoole_event_del)
{
	long fd;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &fd) == FAILURE)
	{
		return;
	}
	SW_CHECK_RETURN(swoole_worker_reactor->del(swoole_worker_reactor, (int)fd));
}

PHP_FUNCTION(swoole_event_exit)
{
	if (php_sw_in_client == 1)
	{
		//stop reactor
		swoole_running = 0;
	}
}

PHP_FUNCTION(swoole_server_addtimer)
{
	zval *zobject = getThis();
	swServer *serv = NULL;
	swFactory *factory = NULL;
	long interval;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ol", &zobject, swoole_server_class_entry_ptr, &interval) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &interval) == FAILURE)
		{
			return;
		}
	}
	SWOOLE_GET_SERVER(zobject, serv);
	SW_CHECK_RETURN(swServer_addTimer(serv, (int)interval));
}

PHP_FUNCTION(swoole_set_process_name)
{
	char *name;
	int name_len;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &name, &name_len) == FAILURE)
	{
		return;
	}
	//it's safe.
#define ARGV_MAX_LENGTH 127
	bzero(sapi_module.executable_location, ARGV_MAX_LENGTH);
	memcpy(sapi_module.executable_location, name, ARGV_MAX_LENGTH-1);
}

PHP_FUNCTION(swoole_server_addcontroller)
{
	zval *zobject = getThis();
	swServer *serv = NULL;
	swFactory *factory = NULL;
	long interval;

	if (zobject == NULL)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ol", &zobject, swoole_server_class_entry_ptr, &interval) == FAILURE)
		{
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &interval) == FAILURE)
		{
			return;
		}
	}
//	SWOOLE_GET_SERVER(zobject, serv);
//	swServer_add_controller();
//	SW_CHECK_RETURN(swServer_addTimer(serv, (int)interval));
}

PHP_METHOD(swoole_client, __construct)
{
	long type, async = 0;
	zval *zres, *errCode, *zsockfd;
	zval *zcallback;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &type, &async) == FAILURE)
	{
		RETURN_FALSE;
	}

	swClient *cli = (swClient*) emalloc(sizeof(swClient));
	if (swClient_create(cli, type, async) < 0)
	{
		zend_error(E_WARNING, "SwooleClient: create fail. Error: %s [%d]", strerror(errno), errno);
		MAKE_STD_ZVAL(errCode);
		ZVAL_LONG(errCode, errno);
		zend_update_property(swoole_client_class_entry_ptr, getThis(), ZEND_STRL("errCode"), errCode TSRMLS_CC);
		RETURN_FALSE;
	}
	MAKE_STD_ZVAL(zres);
	MAKE_STD_ZVAL(zsockfd);
	ZVAL_LONG(zsockfd, cli->sock);

	ZEND_REGISTER_RESOURCE(zres, cli, le_swoole_client);

	zend_update_property(swoole_client_class_entry_ptr, getThis(), ZEND_STRL("sock"), zsockfd TSRMLS_CC);
	zend_update_property(swoole_client_class_entry_ptr, getThis(), ZEND_STRL("_client"), zres TSRMLS_CC);

	RETURN_TRUE;
}

PHP_METHOD(swoole_client, connect)
{
	int ret;
	long port, udp_connect = 0;
	char *host;
	int host_len;
	double timeout = 0.1; //默认100ms超时

	zval **zres;
	zval *errCode;
	swClient *cli = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl|dl", &host, &host_len, &port, &timeout,
			&udp_connect) == FAILURE)
	{
		return;
	}
	if (zend_hash_find(Z_OBJPROP_P(getThis()), SW_STRL("_client"), (void **) &zres) == SUCCESS)
	{
		ZEND_FETCH_RESOURCE(cli, swClient*, zres, -1, SW_RES_CLIENT_NAME, le_swoole_client);
	}
	else
	{
		RETURN_FALSE;
	}

	if(cli->async == 0)
	{
		ret = cli->connect(cli, host, port, (float) timeout, udp_connect);
		if (ret < 0)
		{
			zend_error(E_WARNING, "SwooleClient: connect to server[%s:%d] fail. Error: %s [%d]", host, (int)port, strerror(errno), errno);
			MAKE_STD_ZVAL(errCode);
			ZVAL_LONG(errCode, errno);
			zend_update_property(swoole_client_class_entry_ptr, getThis(), SW_STRL("errCode")-1, errCode TSRMLS_CC);
			RETURN_FALSE;
		}
		else
		{
			RETURN_TRUE;
		}
	}
	//nonblock async
	else
	{
		char *hash_key;
		int hash_key_len;
		int flag = 0;

		hash_key_len = spprintf(&hash_key, 0, "%d", cli->sock);
		zval_add_ref(&getThis());
		if (zend_hash_update(&php_sw_client_callback, hash_key, hash_key_len+1, &getThis(), sizeof(zval*), NULL) == FAILURE)
		{
			zend_error(E_WARNING, "swoole_client: add to hashtable fail");
			RETURN_FALSE;
		}
		php_swoole_check_reactor();
		if (cli->type == SW_SOCK_TCP || cli->type == SW_SOCK_TCP6)
		{
			cli->connect(cli, host, port, (float) timeout, 1);
			flag = (SW_FD_USER+1) | SW_EVENT_WRITE | SW_EVENT_ERROR;
		}
		else
		{
			swEvent ev;
			ev.fd = cli->sock;

			cli->connect(cli, host, port, (float) timeout, udp_connect);
			flag = (SW_FD_USER+1);

			zval *zcallback = NULL;
			zval **args[1];
			zval *retval;

			args[0] = &getThis();
			zcallback = zend_read_property(swoole_client_class_entry_ptr, getThis(), SW_STRL("connect")-1, 0 TSRMLS_CC);
			if (zcallback == NULL)
			{
				zend_error(E_WARNING, "SwooleClient: swoole_client object have not connect callback.");
				RETURN_FALSE;
			}
			if (call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL TSRMLS_CC) == FAILURE)
			{
				zend_error(E_WARNING, "SwooleClient: onConnect[udp] handler error");
				RETURN_FALSE;
			}
		}
		ret = swoole_worker_reactor->add(swoole_worker_reactor, cli->sock, flag);
		php_swoole_try_run_reactor();
		SW_CHECK_RETURN(ret);
	}
}

PHP_METHOD(swoole_client, send)
{
	char *data;
	int data_len;

	zval **zres;
	swClient *cli;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &data, &data_len) == FAILURE)
	{
		return;
	}
	if (zend_hash_find(Z_OBJPROP_P(getThis()), SW_STRL("_client"), (void **) &zres) == SUCCESS)
	{
		ZEND_FETCH_RESOURCE(cli, swClient*, zres, -1, SW_RES_CLIENT_NAME, le_swoole_client);
	}
	else
	{
		RETURN_FALSE;
	}
	SW_CHECK_RETURN(cli->send(cli, data, data_len));
}

PHP_METHOD(swoole_client, recv)
{
	long buf_len = SW_PHP_CLIENT_BUFFER_SIZE, waitall = 0;
	char require_efree = 0;
	char buf_array[SW_PHP_CLIENT_BUFFER_SIZE];
	char *buf;
	zval **zres;
	zval *errCode;
	zval *zretval;

	//zval *zdata;
	int ret;
	swClient *cli;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|ll", &buf_len, &waitall) == FAILURE)
	{
		return;
	}
	if (zend_hash_find(Z_OBJPROP_P(getThis()), SW_STRL("_client"), (void **) &zres) == SUCCESS)
	{
		ZEND_FETCH_RESOURCE(cli, swClient*, zres, -1, SW_RES_CLIENT_NAME, le_swoole_client);
	}
	else
	{
		RETURN_FALSE;
	}

	/**
	 * UDP waitall=0 buf_len小于最大值这3种情况使用栈内存
	 */
	if (cli->type == SW_SOCK_UDP || cli->type == SW_SOCK_UDP6 || waitall == 0 || buf_len < SW_PHP_CLIENT_BUFFER_SIZE)
	{
		buf = buf_array;
		if(buf_len >= SW_PHP_CLIENT_BUFFER_SIZE)  buf_len = SW_PHP_CLIENT_BUFFER_SIZE-1;
	}
	else
	{
		buf = emalloc(buf_len + 1);
		require_efree = 1;
	}

	if ((ret = cli->recv(cli, buf, buf_len, waitall)) < 0)
	{
		//这里的错误信息没用
		zend_error(E_WARNING, "swClient recv fail.Error: %s [%d]", strerror(errno), errno);
		MAKE_STD_ZVAL(errCode);
		ZVAL_LONG(errCode, errno);
		zend_update_property(swoole_client_class_entry_ptr, getThis(), SW_STRL("errCode")-1, errCode TSRMLS_CC);
		RETVAL_FALSE;
	}
	else
	{
//		swWarn("data=%s|ret=%d", buf, ret);
		buf[ret] = 0;
		RETVAL_STRINGL(buf, ret, 1);
	}
	if(require_efree==1) efree(buf);
}

PHP_METHOD(swoole_client, close)
{
	zval **zres;
	zval **zsock;
	swClient *cli;

	if (zend_hash_find(Z_OBJPROP_P(getThis()), SW_STRL("_client"), (void **) &zres) == SUCCESS)
	{
		ZEND_FETCH_RESOURCE(cli, swClient*, zres, -1, SW_RES_CLIENT_NAME, le_swoole_client);
	}
	else
	{
		RETURN_FALSE;
	}
	if(cli->async == 1 && swoole_worker_reactor != NULL)
	{
		swoole_worker_reactor->del(swoole_worker_reactor, cli->sock);
	}
	SW_CHECK_RETURN(cli->close(cli));
}

PHP_METHOD(swoole_client, on)
{
	zval **zres;
	swClient *cli;

	char *cb_name;
	int i, ret, cb_name_len;
	zval *zcallback;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz", &cb_name, &cb_name_len, &zcallback) == FAILURE)
	{
		return;
	}
	if (zend_hash_find(Z_OBJPROP_P(getThis()), SW_STRL("_client"), (void **) &zres) != SUCCESS)
	{
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(cli, swClient*, zres, -1, SW_RES_CLIENT_NAME, le_swoole_client);

	//必须与define顺序一致
	char *callbacks[PHP_CLIENT_CALLBACK_NUM] = {
		"connect",
		"receive",
		"error",
	};
	zval_add_ref(&getThis());
	for(i=0; i<PHP_CLIENT_CALLBACK_NUM; i++)
	{
		if(strncasecmp(callbacks[i], cb_name, cb_name_len) == 0)
		{
			//调用on接口自动修改为异步模式
			if(cli->async == 0)
			{
				cli->async = 1;
			}
			zval_add_ref(&zcallback);
			zend_update_property(swoole_client_class_entry_ptr, getThis(), cb_name, cb_name_len, zcallback TSRMLS_CC);
			RETURN_TRUE;
		}
	}
	zend_error(E_WARNING, "swoole_client: callback[%s] is unknow", cb_name);
	RETURN_FALSE;
}

PHP_FUNCTION(swoole_client_select)
{
	zval *r_array, *w_array, *e_array;
	fd_set rfds, wfds, efds;
	zval *errCode;

	int max_fd = 0;
	int	retval, sets = 0;
	double timeout = 0.5;
	struct timeval timeo;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a!a!a!|d", &r_array, &w_array, &e_array, &timeout) == FAILURE)
	{
		return;
	}
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);

	if (r_array != NULL) sets += php_swoole_client_event_add(r_array, &rfds, &max_fd TSRMLS_CC);
	if (w_array != NULL) sets += php_swoole_client_event_add(w_array, &wfds, &max_fd TSRMLS_CC);
	if (e_array != NULL) sets += php_swoole_client_event_add(e_array, &efds, &max_fd TSRMLS_CC);

	if (!sets)
	{
		zend_error(E_WARNING, "no resource arrays were passed to select");
		RETURN_FALSE;
	}
	if(max_fd >= FD_SETSIZE)
	{
		max_fd = FD_SETSIZE-1;
	}
	timeo.tv_sec = (int) timeout;
	timeo.tv_usec = (int) ((timeout - timeo.tv_sec) * 1000 * 1000);

	retval = select(max_fd + 1, &rfds, &wfds, &efds, &timeo);

	if (retval == -1)
	{
		MAKE_STD_ZVAL(errCode);
		ZVAL_LONG(errCode, errno);
		zend_update_property(swoole_client_class_entry_ptr, getThis(), SW_STRL("errCode")-1, errCode TSRMLS_CC);
		zend_error(E_WARNING, "SwooleClient: unable to select. Error: %s [%d]", strerror(errno), errno);
		RETURN_FALSE;
	}
	if (r_array != NULL)
	{
		php_swoole_client_event_loop(r_array, &rfds TSRMLS_CC);
	}
	if (w_array != NULL)
	{
		php_swoole_client_event_loop(w_array, &wfds TSRMLS_CC);
	}
	if (e_array != NULL)
	{
		php_swoole_client_event_loop(e_array, &efds TSRMLS_CC);
	}
	RETURN_LONG(retval);
}

static int php_swoole_client_event_loop(zval *sock_array, fd_set *fds TSRMLS_DC)
{
	zval **element;
	zval *zsock;
	zval **dest_element;
	swClient *cli;
	HashTable *new_hash;
	zend_class_entry *ce;

	char *key;
	int num = 0;
	ulong num_key;
	uint key_len;

	if (Z_TYPE_P(sock_array) != IS_ARRAY)
	{
		return 0;
	}
	ALLOC_HASHTABLE(new_hash);
	zend_hash_init(new_hash, zend_hash_num_elements(Z_ARRVAL_P(sock_array)), NULL, ZVAL_PTR_DTOR, 0);
	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(sock_array));
	zend_hash_get_current_data(Z_ARRVAL_P(sock_array), (void **) &element) == SUCCESS;
	zend_hash_move_forward(Z_ARRVAL_P(sock_array)))
	{
		ce = Z_OBJCE_P(*element);
		zsock = zend_read_property(ce, *element, SW_STRL("sock")-1, 0 TSRMLS_CC);
		if(!zsock)
		{
			zend_error(E_WARNING, "object is not swoole_client object.");
			continue;
		}
		if ((Z_LVAL(*zsock) < FD_SETSIZE) && FD_ISSET(Z_LVAL(*zsock), fds))
		{
			switch (zend_hash_get_current_key_ex(Z_ARRVAL_P(sock_array), &key, &key_len, &num_key, 0, NULL))
			{
			case HASH_KEY_IS_STRING:
				zend_hash_add(new_hash, key, key_len, (void * )element, sizeof(zval *), (void ** )&dest_element);
				break;
			case HASH_KEY_IS_LONG:
				zend_hash_index_update(new_hash, num_key, (void * )element, sizeof(zval *), (void ** )&dest_element);
				break;
			}
			if (dest_element)
				zval_add_ref(dest_element);
		}
		num++;
	}

	zend_hash_destroy(Z_ARRVAL_P(sock_array));
	efree(Z_ARRVAL_P(sock_array));

	zend_hash_internal_pointer_reset(new_hash);
	Z_ARRVAL_P(sock_array) = new_hash;

	return num ? 1 : 0;
}

static int php_swoole_client_event_add(zval *sock_array, fd_set *fds, int *max_fd TSRMLS_DC)
{
	zval **element;
	zval *zsock;
	swClient *cli;
	zend_class_entry *ce;

	int num = 0;
	if (Z_TYPE_P(sock_array) != IS_ARRAY)
	{
		return 0;
	}
	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(sock_array));
			zend_hash_get_current_data(Z_ARRVAL_P(sock_array), (void **) &element) == SUCCESS;
			zend_hash_move_forward(Z_ARRVAL_P(sock_array)))
	{
		ce = Z_OBJCE_P(*element);
		zsock = zend_read_property(ce, *element, SW_STRL("sock")-1, 0 TSRMLS_CC);
		if(!zsock)
		{
			zend_error(E_WARNING, "object is not swoole_client object.");
			continue;
		}
		if (Z_LVAL(*zsock) < FD_SETSIZE)
		{
			FD_SET(Z_LVAL(*zsock), fds);
		}
		else
		{
			zend_error(E_WARNING, "socket[%ld] > FD_SETSIZE[%d].", Z_LVAL(*zsock), FD_SETSIZE);
			continue;
		}
		if (Z_LVAL(*zsock) > *max_fd)
		{
			*max_fd = Z_LVAL(*zsock);
		}
		num++;
	}
	return num ? 1 : 0;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

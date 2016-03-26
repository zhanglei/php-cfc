/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
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

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_cfc.h"
#include "zend_smart_str.h"
#include <hiredis/hiredis.h>

#define HASH_TABLE_NAME "cfc_hash"

/* If you declare any globals in php_cfc.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(cfc)
*/
static redisContext *g_redis = NULL;

static void (*old_zend_execute_ex)(zend_execute_data *execute_data);

/* True global resources - no need for thread safety here */
static int le_cfc;

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("cfc.enable",      "1", PHP_INI_ALL, OnUpdateBool, global_value, zend_cfc_globals, cfc_globals)
    STD_PHP_INI_ENTRY("cfc.redis_host", "127.0.0.1", PHP_INI_ALL, OnUpdateString, redis_host, zend_cfc_globals, cfc_globals)
    STD_PHP_INI_ENTRY("cfc.redis_port", "6379", PHP_INI_ALL, OnUpdateLong, redis_port, zend_cfc_globals, cfc_globals)
PHP_INI_END()
*/
PHP_INI_BEGIN()
	PHP_INI_ENTRY("cfc.enable", "On", PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("cfc.redis_host", "127.0.0.1", PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("cfc.redis_port", "6379", PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("cfc.prefix", NULL, PHP_INI_ALL, NULL)
PHP_INI_END()
/* }}} */

/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_cfc_compiled(string arg)
   Return a string to confirm that the module is compiled in */

void redis_init()
{
	char *host = INI_STR("cfc.redis_host");
	int port = INI_INT("cfc.redis_port");
    char *msg;

    if (!host) {
        msg = "redis host have not set, using `-r' option";
        goto error;
    }

    g_redis = redisConnect(host, port);
    if (g_redis == NULL || g_redis->err) {
        msg = "Can not connect to redis server";
        goto error;
    }
    return;

error:
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

void redis_free()
{
	redisFree(g_redis);
}

int redis_incr(char *func)
{
	int r = -1;

    redisReply *reply = NULL;
    smart_str command = { 0 };
    smart_str_appends(&command, "HINCRBY ");
    smart_str_appendl(&command, HASH_TABLE_NAME, strlen(HASH_TABLE_NAME));
    smart_str_appendl(&command, " ", strlen(" "));
    smart_str_appendl(&command, func, strlen(func));
    smart_str_appendl(&command, " 1", strlen(" 1"));
    smart_str_0(&command);
    reply = redisCommand(g_redis, command.s->val);
    if (g_redis->err != 0) {
        php_printf("redis hash set failure, error:%d, command:%s\n", g_redis->err, command.s);
    } else {
        r = (int)reply->integer;
    }
    smart_str_free(&command);
    freeReplyObject(reply);
}

PHP_FUNCTION(confirm_cfc_compiled)
{
	char *arg = NULL;
	size_t arg_len, len;
	zend_string *strg;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &arg, &arg_len) == FAILURE) {
		return;
	}

	strg = strpprintf(0, "Congratulations! You have successfully modified ext/%.78s/config.m4. Module %.78s is now compiled into PHP.", "cfc", arg);

	RETURN_STR(strg);
}
/* }}} */
/* The previous line is meant for vim and emacs, so it can correctly fold and
   unfold functions in source code. See the corresponding marks just before
   function definition, where the functions purpose is also documented. Please
   follow this convention for the convenience of others editing your code.
*/


/* {{{ php_cfc_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_cfc_init_globals(zend_cfc_globals *cfc_globals)
{
	cfc_globals->global_value = 0;
	cfc_globals->global_string = NULL;
}
*/
/* }}} */

static char *get_function_name(zend_execute_data * execute_data)
{
	zend_execute_data *data;
	char *ret = NULL;
	int len;
	const char * cls;
	const char * func;
	zend_function *curr_func;
	uint32_t curr_op;
	const zend_op *opline;

	data = EG(current_execute_data);

	if (data)
	{
		curr_func = data->func;
		/* extract function name from the meta info */
		if (curr_func->common.function_name)
		{
			/* previously, the order of the tests in the "if" below was
			 * flipped, leading to incorrect function names in profiler
			 * reports. When a method in a super-type is invoked the
			 * profiler should qualify the function name with the super-type
			 * class name (not the class name based on the run-time type
			 * of the object.
			 */
			func = curr_func->common.function_name->val;
			len  = curr_func->common.function_name->len + 1;
			cls = curr_func->common.scope ?
					curr_func->common.scope->name->val :
					(data->called_scope ?
							data->called_scope->name->val : NULL);
			if (cls)
			{
				len = strlen(cls) + strlen(func) + 10;
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s::%s", cls, func);
			}
			else
			{
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s", func);
			}
		}
		else
		{
			if (data->prev_execute_data)
			{
				opline  = data->prev_execute_data->opline;
			}
			else
			{
				opline  = data->opline;
			}
			switch (opline->extended_value)
			{
			case ZEND_EVAL:
				func = "eval";
				break;
			default:
				func = NULL;
				break;
			}

			if (func)
			{
				ret = estrdup(func);
			}
		}
	}
	return ret;
}

static void my_zend_execute_ex(zend_execute_data *execute_data)
{

	char *func = NULL;
	func = get_function_name(execute_data TSRMLS_CC);
	if (!func) {
		old_zend_execute_ex(execute_data TSRMLS_CC);
		return;
	}
	char *prefix = INI_STR("cfc.prefix");
	if (strncmp(prefix, func, strlen(prefix)) == 0) {
		redis_incr(func);
	}
	old_zend_execute_ex(execute_data TSRMLS_CC);
	efree(func);
}
/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(cfc)
{
	REGISTER_INI_ENTRIES();
	redis_init();
	old_zend_execute_ex = zend_execute_ex;
	zend_execute_ex = my_zend_execute_ex;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(cfc)
{
	UNREGISTER_INI_ENTRIES();
	redis_free();
	zend_execute_ex = old_zend_execute_ex;
	return SUCCESS;
}

/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(cfc)
{
#if defined(COMPILE_DL_CFC) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(cfc)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(cfc)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "cfc support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/* {{{ cfc_functions[]
 *
 * Every user visible function must have an entry in cfc_functions[].
 */
const zend_function_entry cfc_functions[] = {
	PHP_FE(confirm_cfc_compiled,	NULL)		/* For testing, remove later. */
	PHP_FE_END	/* Must be the last line in cfc_functions[] */
};
/* }}} */

/* {{{ cfc_module_entry
 */
zend_module_entry cfc_module_entry = {
	STANDARD_MODULE_HEADER,
	"cfc",
	cfc_functions,
	PHP_MINIT(cfc),
	PHP_MSHUTDOWN(cfc),
	PHP_RINIT(cfc),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(cfc),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(cfc),
	PHP_CFC_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_CFC
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(cfc)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

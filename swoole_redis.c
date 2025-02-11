/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole_async.h"

#include "thirdparty/hiredis/hiredis.h"
#include "thirdparty/hiredis/async.h"

#define SW_REDIS_COMMAND_BUFFER_SIZE   64
#define SW_REDIS_COMMAND_KEY_SIZE      128

typedef struct
{
    redisAsyncContext *context;
    uint8_t state;
    uint8_t connected;
    uint8_t subscribe;
    uint8_t connecting;
    uint32_t reqnum;

    zval *object;
    zval *message_callback;

    double timeout;
    swTimer_node *timer;

    char *password;
    uint8_t password_len;
    int8_t database;
    uint8_t failure;
    uint8_t wait_count;

    zval _message_callback;
    zval _object;

} swRedisClient;

enum swoole_redis_state
{
    SWOOLE_REDIS_STATE_CONNECT,
    SWOOLE_REDIS_STATE_READY,
    SWOOLE_REDIS_STATE_WAIT_RESULT,
    SWOOLE_REDIS_STATE_SUBSCRIBE,
    SWOOLE_REDIS_STATE_CLOSED,
};

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_construct, 0, 0, 0)
    ZEND_ARG_ARRAY_INFO(0, setting, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_connect, 0, 0, 3)
    ZEND_ARG_INFO(0, host)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_call, 0, 0, 2)
    ZEND_ARG_INFO(0, command)
    ZEND_ARG_INFO(0, params)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_on, 0, 0, 2)
    ZEND_ARG_INFO(0, event_name)
    ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_void, 0, 0, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD(swoole_redis, __construct);
static PHP_METHOD(swoole_redis, __destruct);
static PHP_METHOD(swoole_redis, on);
static PHP_METHOD(swoole_redis, connect);
static PHP_METHOD(swoole_redis, getState);
static PHP_METHOD(swoole_redis, __call);
static PHP_METHOD(swoole_redis, close);

static void swoole_redis_onConnect(const redisAsyncContext *c, int status);
static void swoole_redis_onClose(const redisAsyncContext *c, int status);
static int swoole_redis_onRead(swReactor *reactor, swEvent *event);
static int swoole_redis_onWrite(swReactor *reactor, swEvent *event);
static int swoole_redis_onError(swReactor *reactor, swEvent *event);
static void swoole_redis_onResult(redisAsyncContext *c, void *r, void *privdata);
static void swoole_redis_parse_result(swRedisClient *redis, zval* return_value, redisReply* reply);
static void swoole_redis_onCompleted(redisAsyncContext *c, void *r, void *privdata);
static void swoole_redis_onTimeout(swTimer *timer, swTimer_node *tnode);

static void swoole_redis_event_AddRead(void *privdata);
static void swoole_redis_event_AddWrite(void *privdata);
static void swoole_redis_event_DelRead(void *privdata);
static void swoole_redis_event_DelWrite(void *privdata);
static void swoole_redis_event_Cleanup(void *privdata);

static zend_class_entry *swoole_redis_ce;
static zend_object_handlers swoole_redis_handlers;

static const zend_function_entry swoole_redis_methods[] =
{
    PHP_ME(swoole_redis, __construct, arginfo_swoole_redis_construct, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis, __destruct, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis, on, arginfo_swoole_redis_on, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis, connect, arginfo_swoole_redis_connect, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis, close, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis, getState, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis, __call, arginfo_swoole_redis_call, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static sw_inline int swoole_redis_is_message_command(char *command, int command_len)
{
    if (strncasecmp("subscribe", command, command_len) == 0)
    {
        return SW_TRUE;
    }
    else if (strncasecmp("psubscribe", command, command_len) == 0)
    {
        return SW_TRUE;
    }
    else if (strncasecmp("unsubscribe", command, command_len) == 0)
    {
        return SW_TRUE;
    }
    else if (strncasecmp("punsubscribe", command, command_len) == 0)
    {
        return SW_TRUE;
    }
    else
    {
        return SW_FALSE;
    }
}

static sw_inline void redis_execute_connect_callback(swRedisClient *redis, int success)
{
    zval *retval;

    zval args[2];
    zval *zcallback = sw_zend_read_property(swoole_redis_ce, redis->object, ZEND_STRL("onConnect"), 0);

    args[0] = *redis->object;
    ZVAL_BOOL(&args[1], success);

    redis->connecting = 1;
    if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 2, args, 0, NULL) != SUCCESS)
    {
        php_swoole_fatal_error(E_WARNING, "swoole_async_redis connect_callback handler error.");
    }
    if (UNEXPECTED(EG(exception)))
    {
        zend_exception_error(EG(exception), E_ERROR);
    }
    if (retval)
    {
        zval_ptr_dtor(retval);
    }
    redis->connecting = 0;
}

void swoole_redis_init(int module_number)
{
    SW_INIT_CLASS_ENTRY(swoole_redis, "Swoole\\Redis", "swoole_redis", NULL, swoole_redis_methods);
    SW_SET_CLASS_SERIALIZABLE(swoole_redis, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_redis, sw_zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_redis, sw_zend_class_unset_property_deny);

    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("onConnect"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("onClose"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("onMessage"), ZEND_ACC_PUBLIC);

    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("setting"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("host"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("port"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("sock"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("errCode"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_redis_ce, ZEND_STRL("errMsg"), ZEND_ACC_PUBLIC);

    zend_declare_class_constant_long(swoole_redis_ce, ZEND_STRL("STATE_CONNECT"), SWOOLE_REDIS_STATE_CONNECT);
    zend_declare_class_constant_long(swoole_redis_ce, ZEND_STRL("STATE_READY"), SWOOLE_REDIS_STATE_READY);
    zend_declare_class_constant_long(swoole_redis_ce, ZEND_STRL("STATE_WAIT_RESULT"), SWOOLE_REDIS_STATE_WAIT_RESULT);
    zend_declare_class_constant_long(swoole_redis_ce, ZEND_STRL("STATE_SUBSCRIBE"), SWOOLE_REDIS_STATE_SUBSCRIBE);
    zend_declare_class_constant_long(swoole_redis_ce, ZEND_STRL("STATE_CLOSED"), SWOOLE_REDIS_STATE_CLOSED);
}

static PHP_METHOD(swoole_redis, __construct)
{
    zval *zset = NULL;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|z", &zset) == FAILURE)
    {
        RETURN_FALSE;
    }

    swRedisClient *redis = emalloc(sizeof(swRedisClient));
    bzero(redis, sizeof(swRedisClient));

    redis->object = getThis();
    redis->timeout = SW_REDIS_CONNECT_TIMEOUT;
    redis->database = -1;

    if (zset && ZVAL_IS_ARRAY(zset))
    {
        zend_update_property(swoole_redis_ce, getThis(), ZEND_STRL("setting"), zset);

        HashTable *vht;
        zval *ztmp;
        vht = Z_ARRVAL_P(zset);
        /**
         * timeout
         */
        if (php_swoole_array_get_value(vht, "timeout", ztmp))
        {
            redis->timeout = zval_get_double(ztmp);
        }
        /**
         * password
         */
        if (php_swoole_array_get_value(vht, "password", ztmp))
        {
            zend_string *str = zval_get_string(ztmp);
            if (ZSTR_LEN(str) >= 1 << 8)
            {
                php_swoole_fatal_error(E_WARNING, "redis password is too long.");
            }
            else if (ZSTR_LEN(str) > 0)
            {
                redis->password = estrndup(ZSTR_VAL(str), ZSTR_LEN(str));
                redis->password_len = ZSTR_LEN(str);
            }
        }
        /**
         * database
         */
        if (php_swoole_array_get_value(vht, "database", ztmp))
        {
            if (zval_get_long(ztmp) > 1 << 8)
            {
                php_swoole_fatal_error(E_WARNING, "redis database number is too big.");
            }
            else
            {
                redis->database = (int8_t) zval_get_long(ztmp);
            }
        }
    }

    sw_copy_to_stack(redis->object, redis->_object);
    swoole_set_object(getThis(), redis);
}

static PHP_METHOD(swoole_redis, on)
{
    char *name;
    size_t len;
    zval *cb;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sz", &name, &len, &cb) == FAILURE)
    {
        RETURN_FALSE;
    }

    swRedisClient *redis = swoole_get_object(getThis());
    if (redis->context != NULL)
    {
        php_swoole_fatal_error(E_WARNING, "Must be called before connecting.");
        RETURN_FALSE;
    }

    if (strncasecmp("close", name, len) == 0)
    {
        zend_update_property(swoole_redis_ce, getThis(), ZEND_STRL("onClose"), cb);
    }
    else if (strncasecmp("message", name, len) == 0)
    {
        zend_update_property(swoole_redis_ce, getThis(), ZEND_STRL("onMessage"), cb);
        redis->message_callback = sw_zend_read_property(swoole_redis_ce, getThis(), ZEND_STRL("onMessage"), 0);
        sw_copy_to_stack(redis->message_callback, redis->_message_callback);

        redis->subscribe = 1;
    }
    else
    {
        php_swoole_error(E_WARNING, "Unknown event type[%s]", name);
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_redis, connect)
{
    char *host;
    size_t host_len;
    long port;
    zval *callback;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "slz", &host, &host_len, &port, &callback) == FAILURE)
    {
        RETURN_FALSE;
    }

    if (host_len == 0)
    {
        php_swoole_fatal_error(E_WARNING, "redis server host is empty.");
        RETURN_FALSE;
    }

    swRedisClient *redis = swoole_get_object(getThis());
    redisAsyncContext *context;

    if (strncasecmp(host, ZEND_STRL("unix:/")) == 0)
    {
        context = redisAsyncConnectUnix(host + 5);
    }
    else
    {
        if (port <= 1 || port > 65535)
        {
            php_swoole_error(E_WARNING, "redis server port is invalid.");
            RETURN_FALSE;
        }
        context = redisAsyncConnect(host, (int) port);
    }

    if (context == NULL)
    {
        php_swoole_error(E_WARNING, "redisAsyncConnect() failed.");
        RETURN_FALSE;
    }

    if (context->err)
    {
        redisAsyncFree(context);
        php_swoole_error(E_WARNING, "failed to connect to the redis-server[%s:%d], Erorr: %s[%d]", host, (int) port, context->errstr, context->err);
        RETURN_FALSE;
    }

    php_swoole_check_reactor();
    if (!swReactor_isset_handler(SwooleG.main_reactor, PHP_SWOOLE_FD_REDIS))
    {
        swReactor_set_handler(SwooleG.main_reactor, PHP_SWOOLE_FD_REDIS | SW_EVENT_READ, swoole_redis_onRead);
        swReactor_set_handler(SwooleG.main_reactor, PHP_SWOOLE_FD_REDIS | SW_EVENT_WRITE, swoole_redis_onWrite);
        swReactor_set_handler(SwooleG.main_reactor, PHP_SWOOLE_FD_REDIS | SW_EVENT_ERROR, swoole_redis_onError);
    }

    redisAsyncSetConnectCallback(context, swoole_redis_onConnect);
    redisAsyncSetDisconnectCallback(context, swoole_redis_onClose);

    zend_update_property_long(swoole_redis_ce, getThis(), ZEND_STRL("sock"), context->c.fd);
    zend_update_property(swoole_redis_ce, getThis(), ZEND_STRL("onConnect"), callback);

    redis->context = context;
    context->ev.addRead = swoole_redis_event_AddRead;
    context->ev.delRead = swoole_redis_event_DelRead;
    context->ev.addWrite = swoole_redis_event_AddWrite;
    context->ev.delWrite = swoole_redis_event_DelWrite;
    context->ev.cleanup = swoole_redis_event_Cleanup;
    context->ev.data = redis;

    zend_update_property_string(swoole_redis_ce, getThis(), ZEND_STRL("host"), host);
    zend_update_property_long(swoole_redis_ce, getThis(), ZEND_STRL("port"), port);

    if (SwooleG.main_reactor->add(SwooleG.main_reactor, redis->context->c.fd, PHP_SWOOLE_FD_REDIS | SW_EVENT_WRITE) < 0)
    {
        php_swoole_fatal_error(E_WARNING, "swoole_event_add failed. Erorr: %s[%d].", redis->context->errstr, redis->context->err);
        RETURN_FALSE;
    }

    if (redis->timeout > 0)
    {
        redis->timer = swTimer_add(&SwooleG.timer, (long) (redis->timeout * 1000), 0, redis, swoole_redis_onTimeout);
    }

    Z_TRY_ADDREF_P(redis->object);

    swConnection *conn = swReactor_get(SwooleG.main_reactor, redis->context->c.fd);
    conn->object = redis;
}

static void redis_close(void* data)
{
    swRedisClient *redis = data;
    if (redis->context)
    {
        redisAsyncDisconnect(redis->context);
    }
}

static void redis_free_object(void *data)
{
    zval *object = (zval*) data;
    zval_ptr_dtor(object);
}

static void inline redis_free_memory(int argc, char **argv, size_t *argvlen, swRedisClient *redis, zend_bool free_mm)
{
    int i;
    for (i = 1; i < argc; i++)
    {
        efree((void* )argv[i]);
    }

    if (redis->state == SWOOLE_REDIS_STATE_SUBSCRIBE)
    {
        efree(argv[argc]);
    }

    if (free_mm)
    {
        efree(argvlen);
        efree(argv);
    }
}

static PHP_METHOD(swoole_redis, close)
{
    swRedisClient *redis = swoole_get_object(getThis());
    if (redis && redis->context && redis->state != SWOOLE_REDIS_STATE_CLOSED)
    {
        if (redis->connecting)
        {
            SwooleG.main_reactor->defer(SwooleG.main_reactor, redis_close, redis);
        }
        else
        {
            redis_close(redis);
        }
    }
}

static PHP_METHOD(swoole_redis, __destruct)
{
    SW_PREVENT_USER_DESTRUCT();

    swRedisClient *redis = swoole_get_object(getThis());
    if (redis)
    {
        if (redis->context && redis->state != SWOOLE_REDIS_STATE_CLOSED)
        {
            redisAsyncDisconnect(redis->context);
        }
        if (redis->password)
        {
            efree(redis->password);
        }
        efree(redis);
        swoole_set_object(getThis(), NULL);
    }
}

static PHP_METHOD(swoole_redis, __call)
{
    zval *params;
    char *command;
    size_t command_len;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sz", &command, &command_len, &params) == FAILURE)
    {
        RETURN_FALSE;
    }

    if (Z_TYPE_P(params) != IS_ARRAY)
    {
        php_swoole_fatal_error(E_WARNING, "invalid params.");
        RETURN_FALSE;
    }

    swRedisClient *redis = swoole_get_object(getThis());
    if (!redis)
    {
        php_swoole_fatal_error(E_WARNING, "the object is not an instance of swoole_redis.");
        RETURN_FALSE;
    }

    switch (redis->state)
    {
    case SWOOLE_REDIS_STATE_CONNECT:
        php_swoole_error(E_WARNING, "redis client is not connected.");
        RETURN_FALSE;
        break;
    case SWOOLE_REDIS_STATE_WAIT_RESULT:
        if (swoole_redis_is_message_command(command, command_len))
        {
            php_swoole_error(E_WARNING, "redis client is waiting for response.");
            RETURN_FALSE;
        }
        break;
    case SWOOLE_REDIS_STATE_SUBSCRIBE:
        if (!swoole_redis_is_message_command(command, command_len))
        {
            php_swoole_error(E_WARNING, "redis client is waiting for subscribed messages.");
            RETURN_FALSE;
        }
        break;
    case SWOOLE_REDIS_STATE_CLOSED:
        php_swoole_error(E_WARNING, "redis client connection is closed.");
        RETURN_FALSE;
        break;
    default:
        break;
    }

    int argc = zend_hash_num_elements(Z_ARRVAL_P(params));
    size_t stack_argvlen[SW_REDIS_COMMAND_BUFFER_SIZE];
    char *stack_argv[SW_REDIS_COMMAND_BUFFER_SIZE];

    size_t *argvlen;
    char **argv;
    zend_bool free_mm = 0;

    if (argc > SW_REDIS_COMMAND_BUFFER_SIZE)
    {
        argvlen = emalloc(sizeof(size_t) * argc);
        argv = emalloc(sizeof(char*) * argc);
        free_mm = 1;
    }
    else
    {
        argvlen = stack_argvlen;
        argv = stack_argv;
    }


    assert(command_len < SW_REDIS_COMMAND_KEY_SIZE - 1);

    char command_name[SW_REDIS_COMMAND_KEY_SIZE];
    memcpy(command_name, command, command_len);
    command_name[command_len] = '\0';

    argv[0] = command_name;
    argvlen[0] = command_len;

    zval *value;
    int i = 1;

    /**
     * subscribe command
     */
    if (redis->state == SWOOLE_REDIS_STATE_SUBSCRIBE || (redis->subscribe && swoole_redis_is_message_command(command, command_len)))
    {
        redis->state = SWOOLE_REDIS_STATE_SUBSCRIBE;

        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(params), value)
            zend_string *str = zval_get_string(value);
            argvlen[i] = ZSTR_LEN(str);
            argv[i] = estrndup(ZSTR_VAL(str), ZSTR_LEN(str));
            zend_string_release(str);
            if (i == argc)
            {
                break;
            }
            i++;
        SW_HASHTABLE_FOREACH_END();

        if (redisAsyncCommandArgv(redis->context, swoole_redis_onResult, NULL, argc + 1, (const char **) argv, (const size_t *) argvlen) < 0)
        {
            php_swoole_error(E_WARNING, "redisAsyncCommandArgv() failed.");
            redis_free_memory(argc, argv, argvlen, redis, free_mm);
            RETURN_FALSE;
        }
    }
    /**
     * storage command
     */
    else
    {
        redis->state = SWOOLE_REDIS_STATE_WAIT_RESULT;
        redis->reqnum++;

        zval *callback = zend_hash_index_find(Z_ARRVAL_P(params), zend_hash_num_elements(Z_ARRVAL_P(params)) - 1);
        if (callback == NULL)
        {
            php_swoole_error(E_WARNING, "index out of array bounds.");
            redis_free_memory(argc, argv, argvlen, redis, free_mm);
            RETURN_FALSE;
        }

        if (!php_swoole_is_callable(callback))
        {
            redis_free_memory(argc, argv, argvlen, redis, free_mm);
            RETURN_FALSE;
        }

        Z_TRY_ADDREF_P(callback);
        callback = sw_zval_dup(callback);

        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(params), value)
            if (i == argc)
            {
                break;
            }
            zend_string *str = zval_get_string(value);
            argvlen[i] = ZSTR_LEN(str);
            argv[i] = estrndup(ZSTR_VAL(str), ZSTR_LEN(str));
            zend_string_release(str);
            i++;
        SW_HASHTABLE_FOREACH_END();

        if (redisAsyncCommandArgv(redis->context, swoole_redis_onResult, callback, argc, (const char **) argv, (const size_t *) argvlen) < 0)
        {
            php_swoole_error(E_WARNING, "redisAsyncCommandArgv() failed.");
            redis_free_memory(argc, argv, argvlen, redis, free_mm);
            RETURN_FALSE;
        }
    }

    redis_free_memory(argc, argv, argvlen, redis, free_mm);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_redis, getState)
{
    swRedisClient *redis = swoole_get_object(getThis());
    if (!redis)
    {
        php_swoole_fatal_error(E_WARNING, "object is not instanceof swoole_redis.");
        RETURN_FALSE;
    }
    RETURN_LONG(redis->state);
}

static void swoole_redis_set_error(swRedisClient *redis, zval* return_value, redisReply* reply)
{
    char *str = malloc(reply->len + 1);
    memcpy(str, reply->str, reply->len);
    str[reply->len] = 0;

    ZVAL_FALSE(return_value);
    zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"), -1);
    zend_update_property_string(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"), str);
    free(str);
}

static void swoole_redis_parse_result(swRedisClient *redis, zval* return_value, redisReply* reply)
{
    int j;
    zval _val, *val = &_val;

    switch (reply->type)
    {
    case REDIS_REPLY_INTEGER:
        ZVAL_LONG(return_value, reply->integer);
        break;

    case REDIS_REPLY_ERROR:
        swoole_redis_set_error(redis, return_value, reply);
        break;

    case REDIS_REPLY_STATUS:
        if (redis->context->err == 0)
        {
            if (reply->len > 0)
            {
                ZVAL_STRINGL(return_value, reply->str, reply->len);
            }
            else
            {
                ZVAL_TRUE(return_value);
            }
        }
        else
        {
            zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"), redis->context->err);
            zend_update_property_string(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"), redis->context->errstr);
        }
        break;

    case REDIS_REPLY_STRING:
        ZVAL_STRINGL(return_value, reply->str, reply->len);
        break;

    case REDIS_REPLY_ARRAY:
        array_init(return_value);
        for (j = 0; j < reply->elements; j++)
        {
            swoole_redis_parse_result(redis, val, reply->element[j]);
            add_next_index_zval(return_value, val);
        }
        break;

    case REDIS_REPLY_NIL:
    default:
        ZVAL_NULL(return_value);
        return;
    }
}

static void swoole_redis_onTimeout(swTimer *timer, swTimer_node *tnode)
{
    swRedisClient *redis = tnode->data;
    redis->timer = NULL;
    zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"), ETIMEDOUT);
    zend_update_property_string(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"), strerror(ETIMEDOUT));
    redis->state = SWOOLE_REDIS_STATE_CLOSED;
    redis_execute_connect_callback(redis, 0);
    if (redis->context)
    {
        redisAsyncDisconnect(redis->context);
    }
    zval_ptr_dtor(redis->object);
}

static void swoole_redis_onCompleted(redisAsyncContext *c, void *r, void *privdata)
{
    swRedisClient *redis = c->ev.data;
    if (redis->state == SWOOLE_REDIS_STATE_CLOSED)
    {
        return;
    }

    if (redis->failure == 0)
    {
        redisReply *reply = r;
        switch (reply->type)
        {
        case REDIS_REPLY_ERROR:
            zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"), 0);
            zend_update_property_stringl(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"), reply->str,
                    reply->len);
            redis->failure = 1;
            break;

        case REDIS_REPLY_STATUS:
            if (redis->context->err == 0)
            {
                break;
            }
            else
            {
                zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"),
                        redis->context->err);
                zend_update_property_string(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"),
                        redis->context->errstr);
                redis->failure = 1;
            }
            break;
        }
    }

    redis->wait_count--;
    if (redis->wait_count == 0)
    {
        if (redis->failure)
        {
            redis_execute_connect_callback(redis, 0);
            redis->connecting = 0;
            zval *zobject = redis->object;
            sw_zend_call_method_with_0_params(zobject, swoole_redis_ce, NULL, "close", NULL);
            return;
        }
        else
        {
            redis_execute_connect_callback(redis, 1);
        }
    }
}

static void swoole_redis_onResult(redisAsyncContext *c, void *r, void *privdata)
{
    redisReply *reply = r;
    if (reply == NULL)
    {
        return;
    }

    zend_bool is_subscribe = 0;
    char *callback_type;
    swRedisClient *redis = c->ev.data;
    zval result, *retval, *callback;

    swoole_redis_parse_result(redis, &result, reply);

    if (redis->state == SWOOLE_REDIS_STATE_SUBSCRIBE)
    {
        callback = redis->message_callback;
        callback_type = "Message";
        is_subscribe = 1;
    }
    else
    {
        callback = (zval *)privdata;
        callback_type = "Result";
        assert(redis->reqnum > 0 && redis->state == SWOOLE_REDIS_STATE_WAIT_RESULT);
        redis->reqnum--;
        if (redis->reqnum == 0)
        {
            redis->state = SWOOLE_REDIS_STATE_READY;
        }
    }

    zval args[2];
    args[0] = *redis->object;
    args[1] = result;

    if (sw_call_user_function_ex(EG(function_table), NULL, callback, &retval, 2, args, 0, NULL) != SUCCESS)
    {
        php_swoole_fatal_error(E_WARNING, "swoole_redis callback[%s] handler error.", callback_type);
    }
    if (UNEXPECTED(EG(exception)))
    {
        zend_exception_error(EG(exception), E_ERROR);
    }
    if (retval)
    {
        zval_ptr_dtor(retval);
    }
    zval_ptr_dtor(&result);
    if (!is_subscribe)
    {
        sw_zval_free(callback);
    }
}

void swoole_redis_onConnect(const redisAsyncContext *c, int status)
{
    swRedisClient *redis = c->ev.data;

    if (redis->timer)
    {
        swTimer_del(&SwooleG.timer, redis->timer);
        redis->timer = NULL;
    }

    if (status != REDIS_OK)
    {
        zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"), errno);
        zend_update_property_string(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"), c->errstr);
        redis->state = SWOOLE_REDIS_STATE_CLOSED;
        redis_execute_connect_callback(redis, 0);
        SwooleG.main_reactor->defer(SwooleG.main_reactor, redis_free_object, redis->object);
        return;
    }
    else
    {
        redis->state = SWOOLE_REDIS_STATE_READY;
        redis->connected = 1;
    }

    if (redis->password)
    {
        redisAsyncCommand((redisAsyncContext *) c, swoole_redis_onCompleted, NULL, "AUTH %b", redis->password, redis->password_len);
        redis->wait_count++;
    }
    if (redis->database >= 0)
    {
        redisAsyncCommand((redisAsyncContext *) c, swoole_redis_onCompleted, (char*) "end-1", "SELECT %d", redis->database);
        redis->wait_count++;
    }
    if (redis->wait_count == 0)
    {
        redis_execute_connect_callback(redis, 1);
    }
}

void swoole_redis_onClose(const redisAsyncContext *c, int status)
{
    swRedisClient *redis = c->ev.data;
    redis->state = SWOOLE_REDIS_STATE_CLOSED;
    redis->context = NULL;

    zval *zcallback = sw_zend_read_property(swoole_redis_ce, redis->object, ZEND_STRL("onClose"), 1);
    if (zcallback && !ZVAL_IS_NULL(zcallback))
    {
        zval *retval = NULL;
        zval args[1];
        args[0] = *redis->object;
        if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, &retval, 1, args, 0, NULL) != SUCCESS)
        {
            php_swoole_fatal_error(E_WARNING, "swoole_async_redis close_callback handler error.");
        }
        if (UNEXPECTED(EG(exception)))
        {
            zend_exception_error(EG(exception), E_ERROR);
        }
        if (retval)
        {
            zval_ptr_dtor(retval);
        }
    }

    zval_ptr_dtor(redis->object);
}

static int swoole_redis_onError(swReactor *reactor, swEvent *event)
{
    swRedisClient *redis = event->socket->object;
    zval *zcallback = sw_zend_read_property(swoole_redis_ce, redis->object, ZEND_STRL("onConnect"), 0);

    if (!ZVAL_IS_NULL(zcallback))
    {
        const redisAsyncContext *c = redis->context;

        zend_update_property_long(swoole_redis_ce, redis->object, ZEND_STRL("errCode"), c->err);
        zend_update_property_string(swoole_redis_ce, redis->object, ZEND_STRL("errMsg"), c->errstr);

        redis->state = SWOOLE_REDIS_STATE_CLOSED;

        zval args[2];

        args[0] = *redis->object;
        ZVAL_FALSE(&args[1]);

        redis->connecting = 1;
        if (sw_call_user_function_ex(EG(function_table), NULL, zcallback, NULL, 2, args, 0, NULL) != SUCCESS)
        {
            php_swoole_fatal_error(E_WARNING, "swoole_async_redis connect_callback handler error.");
        }
        if (UNEXPECTED(EG(exception)))
        {
            zend_exception_error(EG(exception), E_ERROR);
        }
        redis->connecting = 0;
        zval *zobject = redis->object;
        sw_zend_call_method_with_0_params(zobject, swoole_redis_ce, NULL, "close", NULL);
    }
    return SW_OK;
}

static void swoole_redis_event_AddRead(void *privdata)
{
    swRedisClient *redis = (swRedisClient*) privdata;
    if (redis->context && SwooleG.main_reactor)
    {
        swReactor_add_event(SwooleG.main_reactor, redis->context->c.fd, SW_EVENT_READ);
    }
}

static void swoole_redis_event_DelRead(void *privdata)
{
    swRedisClient *redis = (swRedisClient*) privdata;
    if (redis->context && SwooleG.main_reactor)
    {
        swReactor_del_event(SwooleG.main_reactor, redis->context->c.fd, SW_EVENT_READ);
    }
}

static void swoole_redis_event_AddWrite(void *privdata)
{
    swRedisClient *redis = (swRedisClient*) privdata;
    if (redis->context && SwooleG.main_reactor)
    {
        swReactor_add_event(SwooleG.main_reactor, redis->context->c.fd, SW_EVENT_WRITE);
    }
}

static void swoole_redis_event_DelWrite(void *privdata)
{
    swRedisClient *redis = (swRedisClient*) privdata;
    if (redis->context && SwooleG.main_reactor)
    {
        swReactor_del_event(SwooleG.main_reactor, redis->context->c.fd, SW_EVENT_WRITE);
    }
}

static void swoole_redis_event_Cleanup(void *privdata)
{
    swRedisClient *redis = (swRedisClient*) privdata;
    redis->state = SWOOLE_REDIS_STATE_CLOSED;
    if (redis->context && SwooleG.main_reactor)
    {
        SwooleG.main_reactor->del(SwooleG.main_reactor, redis->context->c.fd);
    }
}

static int swoole_redis_onRead(swReactor *reactor, swEvent *event)
{
    swRedisClient *redis = event->socket->object;
    if (redis->context && SwooleG.main_reactor)
    {
        redisAsyncHandleRead(redis->context);
    }
    return SW_OK;
}

static int swoole_redis_onWrite(swReactor *reactor, swEvent *event)
{
    swRedisClient *redis = event->socket->object;
    if (redis->context && SwooleG.main_reactor)
    {
        redisAsyncHandleWrite(redis->context);
    }
    return SW_OK;
}

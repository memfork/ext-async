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
  | Author: Xinyu Zhu  <xyzhu1120@gmail.com>                             |
  |         shiguangqi <shiguangqi2008@gmail.com>                        |
  |         Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
 */
#include "php_swoole.h"

enum memory_pool_type
{
    memory_pool_type_fixed = 0,
    memory_pool_type_ring = 1,
    memory_pool_type_global = 2,
    memory_pool_type_malloc = 3,
    memory_pool_type_emalloc = 4,
};

static PHP_METHOD(swoole_memory_pool, __construct);
static PHP_METHOD(swoole_memory_pool, __destruct);
static PHP_METHOD(swoole_memory_pool, alloc);
static PHP_METHOD(swoole_memory_pool_slice, read);
static PHP_METHOD(swoole_memory_pool_slice, write);
static PHP_METHOD(swoole_memory_pool_slice, __destruct);

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_void, 0, 0, 0)
ZEND_END_ARG_INFO()

static zend_class_entry *swoole_memory_pool_ce;
static zend_object_handlers swoole_memory_pool_handlers;
static zend_class_entry *swoole_memory_pool_slice_ce;
static zend_object_handlers swoole_memory_pool_slice_handlers;

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_memory_pool_construct, 0, 0, 2)
    ZEND_ARG_INFO(0, size)
    ZEND_ARG_INFO(0, type)
    ZEND_ARG_INFO(0, slice_size)
    ZEND_ARG_INFO(0, shared)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_memory_pool_alloc, 0, 0, 0)
    ZEND_ARG_INFO(0, size)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_memory_pool_slice_read, 0, 0, 0)
    ZEND_ARG_INFO(0, size)
    ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_memory_pool_slice_write, 0, 0, 1)
    ZEND_ARG_INFO(0, data)
    ZEND_ARG_INFO(0, offset)
ZEND_END_ARG_INFO()

typedef struct
{
    size_t size;
    size_t slice_size;
    uint8_t type;
    zend_bool shared;
    zend_bool released;
    swMemoryPool* pool;
    sw_atomic_t slice_count;
} MemoryPool;

typedef struct
{
    size_t size;
    uint8_t type;
    MemoryPool* pool;
    void *memory;
} MemorySlice;

static const zend_function_entry swoole_memory_pool_methods[] =
{
    PHP_ME(swoole_memory_pool, __construct, arginfo_swoole_memory_pool_construct, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_memory_pool, __destruct, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_memory_pool, alloc, arginfo_swoole_memory_pool_alloc, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry swoole_memory_pool_slice_methods[] =
{
    PHP_ME(swoole_memory_pool_slice, read, arginfo_swoole_memory_pool_slice_read, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_memory_pool_slice, write, arginfo_swoole_memory_pool_slice_write, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_memory_pool_slice, __destruct, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void swoole_memory_pool_init(int module_number)
{
    SW_INIT_CLASS_ENTRY(swoole_memory_pool, "Swoole\\Memory\\Pool", "swoole_memory_pool", NULL, swoole_memory_pool_methods);
    SW_SET_CLASS_SERIALIZABLE(swoole_memory_pool, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_memory_pool, sw_zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_memory_pool, sw_zend_class_unset_property_deny);

    SW_INIT_CLASS_ENTRY(swoole_memory_pool_slice, "Swoole\\Memory\\Pool\\Slice", "swoole_memory_pool_slice", NULL, swoole_memory_pool_slice_methods);
    SW_SET_CLASS_SERIALIZABLE(swoole_memory_pool_slice, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_memory_pool_slice, sw_zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_memory_pool_slice, sw_zend_class_unset_property_deny);

    zend_declare_class_constant_long(swoole_memory_pool_ce, ZEND_STRL("TYPE_RING"), memory_pool_type_ring);
    zend_declare_class_constant_long(swoole_memory_pool_ce, ZEND_STRL("TYPE_GLOBAL"), memory_pool_type_global);
    zend_declare_class_constant_long(swoole_memory_pool_ce, ZEND_STRL("TYPE_FIXED"), memory_pool_type_fixed);
    zend_declare_class_constant_long(swoole_memory_pool_ce, ZEND_STRL("TYPE_MALLOC"), memory_pool_type_malloc);
    zend_declare_class_constant_long(swoole_memory_pool_ce, ZEND_STRL("TYPE_EMALLOC"), memory_pool_type_emalloc);
}

static PHP_METHOD(swoole_memory_pool, __construct)
{
    zend_long size, type, slice_size = SW_BUFFER_SIZE_STD;
    zend_bool shared = 0;

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_LONG(size)
        Z_PARAM_LONG(type)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(slice_size)
        Z_PARAM_BOOL(shared)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    swMemoryPool* pool = NULL;
    if (type == memory_pool_type_fixed)
    {
        void *memory = (shared == 1) ? sw_shm_malloc(size) : sw_malloc(size);
        if (memory == NULL)
        {
            zend_throw_exception(swoole_exception_ce, "malloc failed.", SW_ERROR_MALLOC_FAIL);
            RETURN_FALSE;
        }
        pool = swFixedPool_new2(slice_size, memory, size);
    }
    else if (type == memory_pool_type_ring)
    {
        pool = swRingBuffer_new(size, shared);
    }
    else if (type == memory_pool_type_global)
    {
        pool = swMemoryGlobal_new(slice_size, shared);
    }
    else if (type == memory_pool_type_malloc || type == memory_pool_type_malloc)
    {
        shared = 0;
    }
    else
    {
        zend_throw_exception(swoole_exception_ce, "unknown memory pool type.", SW_ERROR_INVALID_PARAMS);
        RETURN_FALSE;
    }

    MemoryPool *mp;
    if (shared)
    {
        mp = (MemoryPool *) SwooleG.memory_pool->alloc(SwooleG.memory_pool, sizeof(MemorySlice));
    }
    else
    {
        mp = (MemoryPool *) emalloc(sizeof(MemorySlice));
    }
    if (mp == NULL)
    {
        zend_throw_exception(swoole_exception_ce, "malloc failed.", SW_ERROR_MALLOC_FAIL);
        RETURN_FALSE;
    }

    mp->size = size;
    mp->slice_size = slice_size;
    mp->shared = shared;
    mp->type = type;
    mp->pool = pool;
    mp->slice_count = 0;
    mp->released = 0;

    swoole_set_object(getThis(), mp);
}

static PHP_METHOD(swoole_memory_pool, alloc)
{
    MemoryPool* mp = (MemoryPool*) swoole_get_object(getThis());
    zend_long size = mp->slice_size;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(size)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    if (mp->type != memory_pool_type_fixed && size <= 0)
    {
        zend_throw_exception(swoole_exception_ce, "invalid size.", SW_ERROR_INVALID_PARAMS);
        RETURN_FALSE;
    }

    void *memory;
    if (mp->type == memory_pool_type_malloc)
    {
        memory = sw_malloc(size);
    }
    else if (mp->type == memory_pool_type_emalloc)
    {
        memory = emalloc(size);
    }
    else
    {
        memory = mp->pool->alloc(mp->pool, size);
    }

    if (memory == NULL)
    {
        RETURN_FALSE;
    }

    MemorySlice *info = (MemorySlice *) emalloc(sizeof(MemorySlice));
    object_init_ex(return_value, swoole_memory_pool_slice_ce);
    info->pool = mp;
    info->size = size;
    info->memory = memory;
    info->type = mp->type;
    sw_atomic_fetch_add(&mp->slice_count, 1);
    swoole_set_object(return_value, info);
}

static PHP_METHOD(swoole_memory_pool, __destruct)
{
    SW_PREVENT_USER_DESTRUCT();

    MemoryPool* mp = (MemoryPool*) swoole_get_object(getThis());
    if (mp == NULL)
    {
        return;
    }

    swoole_set_object(getThis(), NULL);

    if (mp->type == memory_pool_type_malloc || mp->type == memory_pool_type_malloc)
    {
        efree(mp);
        return;
    }

    mp->released = 1;
    if (mp->slice_count == 0)
    {
        mp->pool->destroy(mp->pool);
        if (mp->shared == 0)
        {
            efree(mp);
        }
    }
}

static PHP_METHOD(swoole_memory_pool_slice, read)
{
    zend_long size = 0;
    zend_long offset = 0;

    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(size)
        Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    MemorySlice *info = (MemorySlice *) swoole_get_object(getThis());
    if (size <= 0)
    {
        size = info->size;
    }
    else if (size > info->size)
    {
        size = info->size;
        php_swoole_error(E_WARNING, "size(" ZEND_LONG_FMT ") is too big.", size);
    }

    if (offset < 0 || offset + size > info->size)
    {
        php_swoole_error(E_WARNING, "offset(" ZEND_LONG_FMT ") is out of bounds.", offset);
        RETURN_FALSE;
    }

    RETURN_STRINGL((char * )info->memory + offset, size);
}

static PHP_METHOD(swoole_memory_pool_slice, write)
{
    zend_string *data;
    zend_long size = 0;
    zend_long offset = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(data)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    MemorySlice *info = (MemorySlice *) swoole_get_object(getThis());
    size = ZSTR_LEN(data);
    if (size > info->size)
    {
        php_swoole_error(E_WARNING, "data is too large:" ZEND_LONG_FMT ".", size);
        RETURN_FALSE;
    }
    if (offset < 0 || offset + size > info->size)
    {
        php_swoole_error(E_WARNING, "offset(" ZEND_LONG_FMT ") is out of bounds.", offset);
        RETURN_FALSE;
    }

    memcpy((char *) info->memory + offset, ZSTR_VAL(data), size);

    RETURN_TRUE;
}

static PHP_METHOD(swoole_memory_pool_slice, __destruct)
{
    SW_PREVENT_USER_DESTRUCT();

    MemorySlice *info = (MemorySlice *) swoole_get_object(getThis());
    if (info == NULL)
    {
        return;
    }

    MemoryPool *mp = info->pool;
    if (info->type == memory_pool_type_malloc)
    {
        sw_free(info->memory);
    }
    else if (info->type == memory_pool_type_emalloc)
    {
        efree(info->memory);
    }
    else
    {
        mp->pool->free(mp->pool, info->memory);
        sw_atomic_fetch_sub(&mp->slice_count, 1);

        if (mp->released && mp->slice_count == 0)
        {
            mp->pool->destroy(mp->pool);
            if (mp->shared == 0)
            {
                efree(mp);
            }
        }
    }

    swoole_set_object(getThis(), NULL);
    efree(info);
}

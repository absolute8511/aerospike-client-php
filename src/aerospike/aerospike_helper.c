#include "php.h"
#include "php_aerospike.h"
#include "aerospike/as_log.h"
#include "aerospike/as_key.h"
#include "aerospike/as_config.h"
#include "aerospike/as_error.h"
#include "aerospike/as_status.h"
#include "aerospike/as_record.h"
#include "aerospike/aerospike.h"
#include "pthread.h"
#include "aerospike_common.h"

#define SAVE_PATH_DELIMITER "|"
#define IP_PORT_DELIMITER ":"
#define HOST_DELIMITER ","

/*
 *******************************************************************************************************
 * PHP Userland logger callback.
 *******************************************************************************************************
 */
zend_fcall_info       func_call_info;
zend_fcall_info_cache func_call_info_cache;
zval                  *func_callback_retval_p;
uint32_t              is_callback_registered;

/*
 *******************************************************************************************************
 * aerospike-client-php global log level
 *******************************************************************************************************
 */
#ifndef __AEROSPIKE_PHP_CLIENT_LOG_LEVEL__
as_log_level php_log_level_set = PHP_EXT_AS_LOG_LEVEL_OFF;
#else
as_log_level php_log_level_set = __AEROSPIKE_PHP_CLIENT_LOG_LEVEL__;
#endif

/*
 *******************************************************************************************************
 * Callback for C client's logger.
 * This function shall be invoked by:
 * 1. C client's logger statements.
 * 2. PHP client's logger statements.
 * 
 * @param level             The as_log_level to be used by the callback.
 * @param func              The function name generating the log.
 * @param file              The file name containing the func generating the log.
 * @param line              The line number in file where the log was generated.
 * @param fmt               The format specifier for logger.
 *
 * @return true if log callback succeeds. Otherwise false.
 *******************************************************************************************************
 */
extern bool
aerospike_helper_log_callback(as_log_level level, const char * func TSRMLS_DC, const char * file, uint32_t line, const char * fmt, ...)
{
    char msg[1024] = {0};
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, 1024, fmt, ap);
    msg[1023] = '\0';
    va_end(ap);
    fprintf(stderr, "PHP EXTn: level %d func %s file %s line %d msg %s \n", level, func, file, line, msg);

    if (is_callback_registered) { 
        int16_t   iter = 0;
        zval**    params[4];
        zval*     z_func = NULL;
        zval*     z_file = NULL;
        zval*     z_line = NULL; 
        zval*     z_level = NULL;
        func_callback_retval_p = NULL;

        ALLOC_INIT_ZVAL(z_level);
        ZVAL_LONG(z_level, level);
        params[0] = &z_level;

        ALLOC_INIT_ZVAL(z_func);
        ZVAL_STRING(z_func, func, 1);
        params[1] = &z_func;

        ALLOC_INIT_ZVAL(z_file);
        ZVAL_STRING(z_file, file, 1);
        params[2] = &z_file;

        ALLOC_INIT_ZVAL(z_line);
        ZVAL_LONG(z_line, line);
        params[3] = &z_line;

        func_call_info.param_count = 4;
        func_call_info.params = params;
        func_call_info.retval_ptr_ptr = &func_callback_retval_p;
   
        if (zend_call_function(&func_call_info, &func_call_info_cache TSRMLS_CC) == SUCCESS && 
            func_call_info.retval_ptr_ptr && *func_call_info.retval_ptr_ptr) {
                //TODO: COPY_PZVAL_TO_ZVAL(*return_value, *func_call_info.retval_ptr_ptr);
        } else {
                // TODO: Handle failure in zend_call_function
        }

        for (iter = 0; iter < 4; iter++) {
            zval_ptr_dtor(params[iter]);
        }
    }

    return true;
}

/*
 *******************************************************************************************************
 * Sets the private members error and errorno of the Aerospike class.
 *
 * @param ce_p              The zend_class_entry pointer for the Aerospike class.
 * @param object_p          The Aerospike object.
 * @param error_p           The as_error containing the recent error details.
 * @param reset_flag        The flag indicating whether to set/reset the class error.
 *
 *******************************************************************************************************
 */
extern void
aerospike_helper_set_error(zend_class_entry *ce_p, zval *object_p TSRMLS_DC)
{
    zval*    err_code_p = NULL;
    zval*    err_msg_p = NULL;
    aerospike_global_error error_t = AEROSPIKE_G(error_g);

    MAKE_STD_ZVAL(err_code_p);
    MAKE_STD_ZVAL(err_msg_p);

    if (error_t.reset) {
        ZVAL_STRINGL(err_msg_p, DEFAULT_ERROR, strlen(DEFAULT_ERROR), 1);
        ZVAL_LONG(err_code_p, DEFAULT_ERRORNO);
    } else {
        ZVAL_STRINGL(err_msg_p, error_t.error.message, strlen(error_t.error.message), 1);
        ZVAL_LONG(err_code_p, error_t.error.code);
    }

    zend_update_property(ce_p, object_p, "error", strlen("error"), err_msg_p TSRMLS_CC);
    zend_update_property(ce_p, object_p, "errorno", strlen("errorno"), err_code_p TSRMLS_CC);

    zval_ptr_dtor(&err_code_p);
    zval_ptr_dtor(&err_msg_p);
}

/*
 *******************************************************************************************************
 * This macro is defined to create a new C client's aerospike object.
 *******************************************************************************************************
 */
#define ZEND_CREATE_AEROSPIKE_REFERENCE_OBJECT()                              \
do {                                                                          \
    if (NULL != (as_object_p->as_ref_p = pemalloc(sizeof(aerospike_ref), 1))) \
    {                                                                         \
        as_object_p->as_ref_p->as_p = NULL;                                   \
        as_object_p->as_ref_p->ref_as_p = 0;                                  \
        as_object_p->as_ref_p->ref_hosts_entry = 0;                           \
    }                                                                         \
    as_object_p->as_ref_p->as_p = aerospike_new(conf);                        \
    as_object_p->as_ref_p->ref_as_p = 1;                                      \
} while(0)

/*
 *******************************************************************************************************
 * This macro is defined to register a new resource and to add hash to it.
 *******************************************************************************************************
 */
#define ZEND_HASH_CREATE_ALIAS_NEW(alias, alias_len, new_flag)                 \
do {                                                                           \
    ZEND_CREATE_AEROSPIKE_REFERENCE_OBJECT();                                  \
    ZEND_REGISTER_RESOURCE(rsrc_result, as_object_p->as_ref_p->as_p,           \
            val_persist);                                                      \
    new_le.ptr = as_object_p->as_ref_p;                                        \
    new_le.type = val_persist;                                                 \
    if (new_flag) {                                                            \
        pthread_rwlock_wrlock(&AEROSPIKE_G(aerospike_mutex));                  \
        zend_hash_add(persistent_list, alias, alias_len,                       \
                (void *) &new_le, sizeof(zend_rsrc_list_entry), NULL);         \
        ((aerospike_ref *) new_le.ptr)->ref_hosts_entry++;                     \
        pthread_rwlock_unlock(&AEROSPIKE_G(aerospike_mutex));                  \
    } else {                                                                   \
        pthread_rwlock_wrlock(&AEROSPIKE_G(aerospike_mutex));                  \
        zend_hash_update(persistent_list,                                      \
                alias, alias_len, (void *) &new_le,                            \
                sizeof(zend_rsrc_list_entry), (void **) &le);                  \
        ((aerospike_ref *) new_le.ptr)->ref_hosts_entry++;                     \
        pthread_rwlock_unlock(&AEROSPIKE_G(aerospike_mutex));                  \
    }                                                                          \
} while(0)


/*
 *******************************************************************************************************
 * This macro is defined to match the config details with the stored object
 * details in the resource and if match use the existing one.
 *******************************************************************************************************
 */
#define ZEND_CONFIG_MATCH_USER_STORED(alias, alias_len)                       \
do {                                                                          \
    tmp_ref = le->ptr;                                                        \
    for(itr_user=0; itr_user < conf->hosts_size; itr_user++ ) {               \
        for(itr_stored=0; itr_stored < tmp_ref->as_p->config.hosts_size;      \
                itr_stored++) {                                               \
            if(strlen(conf->hosts[itr_user].addr) ==                          \
                    strlen(tmp_ref->as_p->config.hosts[itr_stored].addr) &&   \
                    (conf->hosts[itr_user].port ==                            \
                     tmp_ref->as_p->config.hosts[itr_stored].port &&          \
                     (strncasecmp(conf->hosts[itr_user].addr,                 \
                                  tmp_ref->as_p->config.hosts[                \
                                  itr_stored].addr,                           \
                                  strlen(conf->hosts[itr_user].addr           \
                                      )) == 0))) {                            \
                goto use_existing;                                            \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    ZEND_HASH_CREATE_ALIAS_NEW(alias, alias_len, 0);                          \
} while(0)

#define MAX_PORT_SIZE 6

/*
 *******************************************************************************************************
 * Function to retrieve a C Client's aerospike object either from the zend
 * persistent store if an already hashed object (with the addr+port as the hash) exists, or by
 * creating a new aerospike object if it doesn't and pushing it on the zend persistent store
 * for further reuse.
 * 
 * @param as_object_p               The instance of Aerospike_object structure containing 
 *                                  the C Client's aerospike object.
 * @param persist_flag              The flag which indicates whether to persist the C Client's 
 *                                  aerospike object.
 * @param conf                      The as_config to be used for creating/retrieving aerospike object.
 * @param persistent_list           The hashtable pointing to the zend global persistent list.
 * @param val_persist               The resource handler for persistent list.
 *
 * @return AEROSPIKE::OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_helper_object_from_alias_hash(Aerospike_object* as_object_p,
                                        bool persist_flag,
                                        as_config* conf,
                                        HashTable *persistent_list,
                                        int val_persist TSRMLS_DC)
{
    zend_rsrc_list_entry *le, new_le;
    zval* rsrc_result = NULL;
    as_status status = AEROSPIKE_OK;
    int itr_user = 0, itr_stored = 0;
    aerospike_ref *tmp_ref = NULL;
    char *alias_to_search = NULL;
    char *alias_to_hash = NULL;
    char port[MAX_PORT_SIZE];

    if (!(as_object_p) && !(conf)) {
        status = AEROSPIKE_ERR_PARAM;
        goto exit;
    }

    if (persist_flag == false) {
        ZEND_CREATE_AEROSPIKE_REFERENCE_OBJECT();
        goto exit;
    }

    /*
     * Iterate over list of hosts and check if the one of them is already
     * hashed and can be reused.
     */

    for (itr_user=0; itr_user < conf->hosts_size; itr_user++) {
        alias_to_search = (char*) emalloc(strlen(conf->hosts[itr_user].addr) + strlen(conf->user) + MAX_PORT_SIZE + 2);
        sprintf(port, "%d", conf->hosts[itr_user].port);
        strcpy(alias_to_search, conf->hosts[itr_user].addr);
        strcat(alias_to_search, ":");
        strcat(alias_to_search, port);
        strcat(alias_to_search, ":");
        strcat(alias_to_search, conf->user);
        pthread_rwlock_rdlock(&AEROSPIKE_G(aerospike_mutex));
        if (zend_hash_find(persistent_list, alias_to_search,
                strlen(alias_to_search), (void **) &le) == SUCCESS) {
            if (alias_to_search) {
                efree(alias_to_search);
                alias_to_search = NULL;
            }
            pthread_rwlock_unlock(&AEROSPIKE_G(aerospike_mutex));
            tmp_ref = le->ptr;
            goto use_existing;
        }
        pthread_rwlock_unlock(&AEROSPIKE_G(aerospike_mutex));
        if (alias_to_search) {
            efree(alias_to_search);
            alias_to_search = NULL;
        }
    }

    alias_to_search = (char*) emalloc(strlen(conf->hosts[0].addr) + strlen(conf->user) + MAX_PORT_SIZE + 2);
    sprintf(port, "%d", conf->hosts[0].port);
    strcpy(alias_to_search, conf->hosts[0].addr);
    strcat(alias_to_search, ":");
    strcat(alias_to_search, port);
    strcat(alias_to_search, ":");
    strcat(alias_to_search, conf->user);
    ZEND_HASH_CREATE_ALIAS_NEW(alias_to_search, strlen(alias_to_search), 1);

    /*
     * Iterate over remaining list of hosts and hash them into the persistent
     * list, each pointing to the same aerospike_ref object.
     * Increment corresponding ref_hosts_entry within the aerospike_ref object.
     */

    for (itr_user=1; itr_user < conf->hosts_size; itr_user++ ) {
        alias_to_hash = (char*) emalloc(strlen(conf->hosts[itr_user].addr) + strlen(conf->user) + MAX_PORT_SIZE + 2);
        sprintf(port, "%d", conf->hosts[itr_user].port);
        strcpy(alias_to_hash, conf->hosts[itr_user].addr);
        strcat(alias_to_hash, ":");
        strcat(alias_to_hash, port);
        strcat(alias_to_hash, ":");
        strcat(alias_to_hash, conf->user);
        pthread_rwlock_wrlock(&AEROSPIKE_G(aerospike_mutex));
        zend_hash_add(persistent_list, alias_to_hash,
                strlen(alias_to_hash), (void *) &new_le, sizeof(zend_rsrc_list_entry), NULL);
        ((aerospike_ref *) new_le.ptr)->ref_hosts_entry++;
        pthread_rwlock_unlock(&AEROSPIKE_G(aerospike_mutex));
        efree(alias_to_hash);
        alias_to_hash = NULL;
    }
    goto exit;

use_existing:
    /*
     * config details have matched, use the existing one obtained from the
     * storage.
     * Increment corresponding ref_as_p of the aerospike_ref object.
     */
    as_object_p->is_conn_16 = AEROSPIKE_CONN_STATE_TRUE;
    as_object_p->as_ref_p = tmp_ref;
    as_object_p->as_ref_p->ref_as_p++;
    DEBUG_PHP_EXT_DEBUG("\nCount is: %d",as_object_p->as_ref_p->ref_as_p);
    goto exit;
exit:
    if (alias_to_search) {
        efree(alias_to_search);
        alias_to_search = NULL;
    }
    return (status);
}

/*
 *******************************************************************************************************
 * Function to destroy all as_* types initiated within the as_static_pool.
 * To be called if as_static_pool has been initialized after the use of pool is
 * complete.
 *
 * @param static_pool               The as_static_pool object to be freed.
 *******************************************************************************************************
 */
extern void
aerospike_helper_free_static_pool(as_static_pool *static_pool)
{
    uint32_t iter = 0;

    /* clean up the as_* objects that were initialised */
    for (iter = 0; iter < static_pool->current_str_id; iter++) {
        as_string_destroy(&static_pool->string_pool[iter]);
    }

    for (iter = 0; iter < static_pool->current_int_id; iter++) {
        as_integer_destroy(&static_pool->integer_pool[iter]);
    }

    for (iter = 0; iter < static_pool->current_bytes_id; iter++) {
        as_bytes_destroy(&static_pool->bytes_pool[iter]);
    }

    for (iter = 0; iter < static_pool->current_list_id; iter++) {
        as_arraylist_destroy(&static_pool->alloc_list[iter]);
    }

    for (iter = 0; iter < static_pool->current_map_id; iter++) {
        as_hashmap_destroy(&static_pool->alloc_map[iter]);
    }
}

/*
 *******************************************************************************************************
 * Callback for as_scan_foreach and as_query_foreach functions.
 * It processes the as_val and translates it into an equivalent zval array.
 * It then calls the user registered callback passing the zval array as an
 * argument.
 *
 * @param p_val             The current as_val to be passed on to the user
 *                          callback as an argument.
 * @param udata             The userland_callback instance filled with fci and
 *                          fcc.
 * @return true if callback is successful; else false.
 *******************************************************************************************************
 */

extern bool
aerospike_helper_record_stream_callback(const as_val* p_val, void* udata)
{
    as_status               status = AEROSPIKE_OK;
    as_error                error;
    zval                    *record_p = NULL;
    zval                    **args[1];
    zval                    *retval = NULL;
    bool                    do_continue = true;
    foreach_callback_udata  foreach_record_callback_udata;
    zval                    *outer_container_p = NULL;
    userland_callback       *user_func_p = (userland_callback *) udata;

    TSRMLS_FETCH_FROM_CTX(user_func_p->ts);

    if (!p_val) {
        DEBUG_PHP_EXT_INFO("callback is null; stream complete.");
        return true;
    }
    as_record* current_as_rec = as_record_fromval(p_val);
    if (!current_as_rec) {
        DEBUG_PHP_EXT_WARNING("stream returned a non-as_record object to the callback.");
        return true;
    }

    pthread_rwlock_wrlock(&AEROSPIKE_G(query_cb_mutex));
    MAKE_STD_ZVAL(record_p);
    array_init(record_p);

    foreach_record_callback_udata.udata_p = record_p;
    foreach_record_callback_udata.error_p = &error;
    if (!as_record_foreach(current_as_rec, (as_rec_foreach_callback) AS_DEFAULT_GET,
        &foreach_record_callback_udata)) {
        DEBUG_PHP_EXT_WARNING("stream callback failed to transform the as_record to an array zval.");
        zval_ptr_dtor(&record_p);
        pthread_rwlock_unlock(&AEROSPIKE_G(query_cb_mutex));
        return true;
    }

    MAKE_STD_ZVAL(outer_container_p);
    array_init(outer_container_p);

    if (AEROSPIKE_OK != (status = aerospike_get_key_meta_bins_of_record(NULL, current_as_rec,
                    &(current_as_rec->key), outer_container_p, NULL, false TSRMLS_CC))) {
        DEBUG_PHP_EXT_DEBUG("Unable to get a record and metadata");
        zval_ptr_dtor(&record_p);
        zval_ptr_dtor(&outer_container_p);
        pthread_rwlock_unlock(&AEROSPIKE_G(query_cb_mutex));
        return true;
    }

    if (0 != add_assoc_zval(outer_container_p, PHP_AS_RECORD_DEFINE_FOR_BINS, record_p)) {
        DEBUG_PHP_EXT_DEBUG("Unable to get a record");
        zval_ptr_dtor(&record_p);
        zval_ptr_dtor(&outer_container_p);
        pthread_rwlock_unlock(&AEROSPIKE_G(query_cb_mutex));
        return true;
    }

    /*
     * Call the userland function with the array representing the record.
     */

    args[0] = &outer_container_p;
    user_func_p->fci.param_count = 1;
    user_func_p->fci.params = args;
    user_func_p->fci.retval_ptr_ptr = &retval;

    if (zend_call_function(&user_func_p->fci, &user_func_p->fcc TSRMLS_CC) == FAILURE) {
        DEBUG_PHP_EXT_WARNING("stream callback could not invoke the userland function.");
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "stream callback could not invoke userland function.");
        zval_ptr_dtor(&outer_container_p);
        pthread_rwlock_unlock(&AEROSPIKE_G(query_cb_mutex));
        return true;
    }

    zval_ptr_dtor(&outer_container_p);

    if (retval) {
        if ((Z_TYPE_P(retval) == IS_BOOL) && !Z_BVAL_P(retval)) {
            do_continue = false;
        } else {
            do_continue = true;
        }
        zval_ptr_dtor(&retval);
    }
    pthread_rwlock_unlock(&AEROSPIKE_G(query_cb_mutex));
    return do_continue;
}

/*
 *******************************************************************************************************
 * Callback for as_query_foreach function in case of Aerospike::aggregate().
 * It processes the as_val and translates it into an equivalent zval.
 * It then populates the return zval with the same.
 *
 * @param val_p             The current as_val to be passed on to the user
 *                          callback as an argument.
 * @param udata_p           The zval return value to be filled with the result
 *                          of aggregation.
 * @return true if callback is successful; else false.
 *******************************************************************************************************
 */
extern bool
aerospike_helper_aggregate_callback(const as_val* val_p, void* udata_p)
{
    TSRMLS_FETCH();
    if (!val_p) {
        DEBUG_PHP_EXT_INFO("callback is null; stream complete.");
        return true;
    }

    AS_AGGREGATE_GET(NULL, val_p, (foreach_callback_udata *) udata_p);
exit:
    return true;
}

extern void
aerospike_helper_check_and_configure_shm(as_config *config_p TSRMLS_DC) {
    if (SHM_USE_PHP_INI == 1) {
        config_p->use_shm = true;
        config_p->shm_max_nodes = (uint32_t) SHM_MAX_NODES_PHP_INI;
        config_p->shm_max_namespaces = (uint32_t) SHM_MAX_NAMESPACES_PHP_INI;
        config_p->shm_takeover_threshold_sec = (uint32_t) SHM_TAKEOVER_THRESHOLD_SEC_PHP_INI;
    } else {
        config_p->use_shm = false;
    }
}

/*
 *******************************************************************************************************
 * Function called from Aerospike::close().
 * It decrements ref_as_p which indicates the no. of references for internal C
 * SDK aerospike object being held by the various PHP userland Aerospike
 * objects.
 * It DOES NOT actually close the connection to server or free as_ref_p as other
 * PHP userland aerospike objects may re-use it in future.
 *
 * @param as_obj_p          The Aerospike_object upon which close() is invoked
 *                          currently.
 * @param error_p           The C SDK's as_error object to be populated by this
 *                          method in case of any errors if encountered.
 *
 * @return AEROSPIKE::OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_helper_close_php_connection(Aerospike_object *as_obj_p,
        as_error *error_p TSRMLS_DC) 
{
    as_error_init(error_p);
    DEBUG_PHP_EXT_DEBUG("In aerospike_helper_close_php_connection");
    if (as_obj_p->as_ref_p) {
        if (as_obj_p->as_ref_p->ref_as_p >= 1) {
            as_obj_p->as_ref_p->ref_as_p--;
        } else if (as_obj_p->as_ref_p->ref_as_p <= 0) {
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT,
                    "Connection already closed!");
            DEBUG_PHP_EXT_ERROR("Connection already closed!");
        }
        DEBUG_PHP_EXT_INFO("Connection successfully closed!");
    } else {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT,
                "Connection already closed and destroyed!");
        DEBUG_PHP_EXT_ERROR("Connection already closed and destroyed!");
    }
    return error_p->code;
}

/*
 *******************************************************************************************************
 * Function that trims leading and trailing white spaces in a given string.
 *
 * @param str               The input string to be trimmed.
 * @param len               The length of input string.
 *
 * @return len of trimmed str string on success. Otherwise 1.
 *******************************************************************************************************
 */
static int
trim_white_space(const char *str, size_t len)
{
    if (len == 0) {
        return 0;
    }

    const char *end;
    int out_size;

    /* Trim leading space */
    while(isspace(*str)) {
        str++;
    }

    if(*str == 0) {
        /* All spaces? */
        return 1;
    }

    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while(end > str && isspace(*end)) {
        end--;
    }
    end++;

    /* Set output size to minimum of trimmed string length and buffer size minus 1 */
    out_size = (end - str) < len-1 ? (end - str) : len-1;

    /* Copy trimmed string and add null terminator */

    return out_size;
}

/*
 *******************************************************************************************************
 * Function to parse the session save path.
 * It sets the ns and set in aerospike_session by allocating memory to it.
 * It also sets the host within as_config.
 *
 * @param save_path         The session save path to be parsed.
 * @param session_p         The aerospike_session object whose ns_p and set_p
 *                          are to be set using the save_path.
 * @param config_p          The as_config whose hosts are to be set using the
 *                          save_path.
 * @param error_p           The C SDK's as_error object to be populated by this
 *                          method in case of any errors if encountered.
 *
 * @return AEROSPIKE::OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
static as_status
parse_save_path(char *save_path, aerospike_session *session_p,
        as_config *config_p, as_error *error_p TSRMLS_DC)
{
    char        *tok = NULL;
    char        *saved = NULL;
    char        port[INET_PORT];
    int16_t     iter_host = 0;
    char        *copy = NULL;
    
    copy = (char *) emalloc(strlen(save_path) + 1);
    strncpy(copy, save_path, strlen(save_path) + 1);
    
    tok = strtok_r(copy, SAVE_PATH_DELIMITER, &saved);
    
    if (tok == NULL) {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT,
                "Could not read SAVE_PATH settings");
        DEBUG_PHP_EXT_DEBUG("Could not read SAVE_PATH settings");
        goto exit;
    }

    strncpy(session_p->ns_p, tok, strlen(tok));
    session_p->ns_p[strlen(tok)] = '\0';

    tok = strtok_r(NULL, SAVE_PATH_DELIMITER, &saved);
    if (tok == NULL) {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT,
                "Could not read SAVE_PATH settings");
        DEBUG_PHP_EXT_DEBUG("Could not read SAVE_PATH settings");
        goto exit;
    }

    strncpy(session_p->set_p, tok, strlen(tok));
    session_p->set_p[strlen(tok)] = '\0';

    while (tok != NULL) {
        tok = strtok_r(NULL, IP_PORT_DELIMITER, &saved);
        if (tok == NULL) {
            if (iter_host == 0) {
                PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT,
                        "Could not read SAVE_PATH settings");
                DEBUG_PHP_EXT_DEBUG("Could not read SAVE_PATH settings");
                goto exit;
            } else {
                break;
            }
        }
        trim_white_space(tok, strlen(tok) + 1);
        char *addr = (char *) emalloc(strlen(tok) + 1);
        strncpy(addr, tok, strlen(tok) + 1);
        config_p->hosts[iter_host].addr = addr;

        tok = strtok_r(NULL, HOST_DELIMITER, &saved);
        if (tok == NULL) {
            if (iter_host == 0) {
                PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT,
                        "Could not read SAVE_PATH settings");
                DEBUG_PHP_EXT_DEBUG("Could not read SAVE_PATH settings");
                goto exit;
            } else {
                break;
            }
        }
        trim_white_space(tok, strlen(tok) + 1);
        config_p->hosts[iter_host].port = atoi(tok);
        config_p->hosts_size++;
        iter_host++;
    }

exit:
    if (copy) {
        efree(copy);
    }
    return error_p->code;
}

/*
 *******************************************************************************************************
 * Function to check save path and set config and session object.
 * It uses the save path passed to it. If NULL, checks for PHP INI entries.
 * It sets the ns and set in aerospike_session by allocating memory to it.
 * It also sets the host within as_config.
 *
 * @param config_p          The as_config whose hosts are to be set using the
 *                          save_path.
 * @param save_path         The session save path to be parsed.
 * @param session_p         The aerospike_session object whose ns_p and set_p
 *                          are to be set using the save_path.
 * @param error_p           The C SDK's as_error object to be populated by this
 *                          method in case of any errors if encountered.
 *
 * @return AEROSPIKE::OK if success. Otherwise AEROSPIKE_x.
 *******************************************************************************************************
 */
extern as_status
aerospike_helper_check_and_set_config_for_session(as_config *config_p,
        char *save_path, aerospike_session *session_p,
        as_error *error_p TSRMLS_DC)
{
    char        *ip = NULL;
    uint16_t    port = 0;

    as_error_init(error_p);

    as_config_init(config_p);
    strcpy(config_p->lua.system_path, LUA_SYSTEM_PATH_PHP_INI);
    strcpy(config_p->lua.user_path, LUA_USER_PATH_PHP_INI);

    if (!strncmp(SAVE_HANDLER_PHP_INI, AEROSPIKE_SESSION, AEROSPIKE_SESSION_LEN)) {
        if (!save_path) {
            save_path = SAVE_PATH_PHP_INI;
        }

        if (save_path) {
            if (AEROSPIKE_OK != parse_save_path(save_path, session_p,
                        config_p, error_p TSRMLS_CC)) {
                goto exit;
            }
        } else {
            PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT, "Could not read SAVE_PATH settings");
            DEBUG_PHP_EXT_ERROR("Could not read SAVE_PATH settings");
            goto exit;
        }
    } else {
        PHP_EXT_SET_AS_ERR(error_p, AEROSPIKE_ERR_CLIENT, "Could not read SAVE_HANDLER settings");
        DEBUG_PHP_EXT_ERROR("Could not read SAVE_HANDLER settings");
        goto exit;
    }

exit:
    return error_p->code;
}

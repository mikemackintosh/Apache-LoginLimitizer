/*
   mod_llzr 0.1
   Copyright (C) 2013

    Author: Mike Mackintosh < m [at] zyp [dot] io >
   Website: www.mikemackintosh.com

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Instalation:
   - /usr/apache/bin/apxs -a -i -l cap -c mod_llzr.c
*/

#include "httpd.h"
#include "http_config.h"
#include "http_connection.h"
#include "http_log.h"
#include "ap_mpm.h"
#include "apr_strings.h"
#include "scoreboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hiredis.h>

#define MODULE_NAME "mod_llzr"
#define MODULE_VERSION "0.1b"

module AP_MODULE_DECLARE_DATA llzr_module;

static int server_limit, thread_limit;

#define LLZR_MAXPERIP 10
#define LLZR_REDISHOST "127.0.0.1"
#define LLZR_REDISPORT 6379

typedef struct
{
    signed int limit;
    char *redis_host;
    signed int redis_port;
    redisContext *redisconn;
    redisReply *redisreply;
} llzr_config;

typedef struct {
    int child_num;
    int thread_num;
} sb_handle;

typedef struct {
    const char* key;
    const char* value;
} keyValuePair;

keyValuePair* readPost(request_rec* r) {
    apr_array_header_t *pairs = NULL;
    apr_off_t len;
    apr_size_t size;
    int res;
    int i = 0;
    char *buffer;
    keyValuePair* kvp;

    res = ap_parse_form_data(r, NULL, &pairs, -1, HUGE_STRING_LEN);
    if (res != OK || !pairs) return NULL; /* Return NULL if we failed or if there are is no POST data */
    kvp = apr_pcalloc(r->pool, sizeof(keyValuePair) * (pairs->nelts + 1));
    while (pairs && !apr_is_empty_array(pairs)) {
        i++;
        ap_form_pair_t *pair = (ap_form_pair_t *) apr_array_pop(pairs);
        apr_brigade_length(pair->value, 1, &len);
        size = (apr_size_t) len;
        buffer = apr_palloc(r->pool, size + 1);
        apr_brigade_flatten(pair->value, buffer, &size);
        buffer[len] = 0;
        kvp[i]->key = apr_pstrdup(r->pool, pair->name);
        kvp[i]->value = buffer;
    }
    return kvp;
}

/* Create per-server configuration structure */
static void *create_config(apr_pool_t *p, server_rec *s)
{
    llzr_config *conf = apr_pcalloc(p, sizeof (*conf));

    /* Set defined defaults */
    conf->limit = LLZR_MAXPERIP;
    conf->redis_host = LLZR_REDISHOST;
    conf->redis_port = LLZR_REDISPORT;

    return conf;
}


/* Parse the LLZRLimit directive */
static const char *limit_config_cmd(cmd_parms *parms, void *mconfig, const char *arg)
{
    llzr_config *conf = ap_get_module_config(parms->server->module_config, &llzr_module);
    const char *err = ap_check_cmd_context (parms, GLOBAL_ONLY);

    if (err != NULL) {
    return err;
    }

    /* Cast limit from str to long */
    signed long int limit = strtol(arg, (char **) NULL, 10);

    /* No reasonable person would want more than 2^16. Better would be
       to use LONG_MAX but that causes portability problems on win32 */
    if ((limit > 65535) || (limit < 0)) {
        return "Integer overflow or invalid number";
    }

    conf->limit = limit;
    return NULL;
}


/* Parse the LLZRRedisServer directive */
static const char *redis_host_cmd(cmd_parms *parms, void *mconfig, const char *arg)
{
    llzr_config *conf = ap_get_module_config(parms->server->module_config, &llzr_module);
    const char *err = ap_check_cmd_context (parms, GLOBAL_ONLY);

    if (err != NULL) {
        return err;
    }

    /* Need to add validation here */
    conf->redis_host = arg;

    return NULL;
}


/* Parse the LLZRRedisServer directive */
static const char *redis_port_cmd(cmd_parms *parms, void *mconfig, const char *arg)
{
    llzr_config *conf = ap_get_module_config(parms->server->module_config, &llzr_module);
    const char *err = ap_check_cmd_context (parms, GLOBAL_ONLY);

    if (err != NULL) {
        return err;
    }

    /* Cast port from str to long */
    signed long int port = strtol(arg, (char **) NULL, 10);

    /* Again, no port should be more than 65535 or <= 0 */
    if ((port > 65535) || (port < 0)) {
        return "Integer overflow or invalid number";
    }

    conf->redis_port = port;

    return NULL;
}


/*
 * Basic Usage:
 *
 * ...
 * LLZRLimit 10
 * LLZRRedisServer 127.0.0.1
 * LLZRRedisPort 6379
 * ...
 *
 */

/* Array describing structure of configuration directives */
static command_rec llzr_cmds[] = {
    AP_INIT_TAKE1("LLZRLimit", limit_config_cmd, NULL, RSRC_CONF, "Maximum simultaneous connections per IP address"),
    AP_INIT_TAKE1("LLZRRedisServer", redis_host_cmd, NULL, RSRC_CONF, "Redis Host to Connect To"),
    AP_INIT_TAKE1("LLZRRedisPort", redis_port_cmd, NULL, RSRC_CONF, "Redis Port to Connect To"),
    {NULL}
};


/* Set up startup-time initialization */
static int post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    void *data;
    const char *userdata_key = "llzr_init";
    llzr_config *conf = ap_get_module_config(parms->server->module_config, &llzr_module);

    conf->redisconn = redisConnectWithTimeout(hostname, port, timeout);
    if (conf->redisconn == NULL || conf->redisconn->err) {
        if (conf->redisconn) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, "Redis error encountered %s", conf->redisconn->errstr);
            redisFree(c);
        } else {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, "Unable to connect to redis server");
        }

        /* We don't want to kill our site if Redis goes down, but we would be susceptible to attack */
    }

    apr_pool_userdata_get(&data, userdata_key, s->process->pool);
    if (!data) {
	   apr_pool_userdata_set((const void *)1, userdata_key,apr_pool_cleanup_null, s->process->pool);
	       return OK;
    }

    /* Lets get details from the MPM */
    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL, MODULE_NAME " " MODULE_VERSION " started");
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &thread_limit);
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_DAEMONS, &server_limit);

    return OK;
}


static int pre_connection(conn_rec *c)
{
    llzr_config *conf = ap_get_module_config (c->base_server->module_config,  &llzr_module);
    sb_handle *sbh = c->sbh;
    redis = conf->redisconn;
    reply = conf->redisreply;

    /* loop index variables */
    int i;
    int j;

    /* running count of number of connections from this address */
    int ip_count = 0;

    /* scoreboard data structure */
    worker_score *ws_record;

    ws_record = &ap_scoreboard_image->servers[sbh->child_num][sbh->thread_num];
    apr_cpystrn(ws_record->client, c->remote_ip, sizeof(ws_record->client));

    char *client_ip = ws_record->client;

    /* Count up the number of connections we are handling right now from this IP address */
    for (i = 0; i < server_limit; ++i) {
	for (j = 0; j < thread_limit; ++j) {
    	    ws_record = ap_get_scoreboard_worker(i, j);
            switch (ws_record->status) {
        	case SERVER_BUSY_READ:
            	    if (strcmp(client_ip, ws_record->client) == 0)
            		ip_count++;
                    break;
                default:
            	    break;
            }
        }
    }

    if (ip_count > conf->limit) {
	   ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, "Rejected, too many connections in READ state from %s", c->remote_ip);
	return OK;
    } else {
	   return DECLINED;
    }
}

static int check_contents(request_rec *r)
{
    llzr_config *conf = ap_get_module_config (c->base_server->module_config,  &llzr_module);
    keyValuePair* formData;
    redis = conf->redisconn;
    reply = conf->redisreply;

    /*
        Things of interest:

        request_rec->uri
        request_rec->useragent_ip
        request_rec->request_time
        request_rec->method

    */

   /*

        @TODO: look through post request for username and password

        if they are included, create a token id, and insert into redis database
        send back a 402 payment required or 307 to /login/token
        ask the user to authorize login...
        on resubmission ony the token is passed and if exists, login is sucessful
        if not, the entry is logged and evaluated

   */

    formData = readPost(r);
    if (formData) {
        int i;
        for (i = 0; formData[i]; i++) {
            ap_rprintf(r, "%s = %s\n", formData[i]->key, formData[i]->value);
        }
    }
    return OK;
}

static void child_init (apr_pool_t *p, server_rec *s)
{
    ap_add_version_component(p, MODULE_NAME "/" MODULE_VERSION);
}


static void register_hooks(apr_pool_t *p)
{
    ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_process_connection(pre_connection, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_child_init(child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(check_contents, NULL, NULL, APR_HOOK_LAST);
}

module AP_MODULE_DECLARE_DATA llzr_module = {
    STANDARD20_MODULE_STUFF,
    NULL,			/* create per-dir config structures */
    NULL,			/* merge  per-dir    config structures */
    create_config,		/* create per-server config structures */
    NULL,			/* merge  per-server config structures */
    llzr_cmds,		/* table of config file commands       */
    register_hooks
};

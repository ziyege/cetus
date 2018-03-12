/** @file
 * the user-interface for the cetus @see main()
 *
 *  -  command-line handling
 *  -  config-file parsing
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <malloc.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h> /* for rusage in wait() */

#include <glib.h>
#include <gmodule.h>

#ifdef HAVE_VALGRIND_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

#ifdef HAVE_SIGACTION
#include <execinfo.h>
#endif

#ifndef HAVE_VALGRIND_VALGRIND_H
#define RUNNING_ON_VALGRIND 0
#endif

#include "glib-ext.h"
#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "sys-pedantic.h"

#include "cetus-log.h"
#include "chassis-log.h"
#include "chassis-keyfile.h"
#include "chassis-mainloop.h"
#include "chassis-path.h"
#include "chassis-limits.h"
#include "chassis-filemode.h"
#include "chassis-unix-daemon.h"
#include "chassis-frontend.h"
#include "chassis-options.h"
#include "cetus-monitor.h"

#define GETTEXT_PACKAGE "cetus"

/**
 * options of the cetus frontend
 */
typedef struct {
    int print_version;
    int verbose_shutdown;

    int daemon_mode;
    int set_client_found_rows;
    int default_pool_size;
    int max_pool_size;
    int merged_output_size;
    int max_header_size;
    int max_resp_len;
    int master_preferred;
    int worker_id;
    int config_port;
    int disable_threads;
    int is_tcp_stream_enabled;
    int is_back_compressed;
    int is_client_compress_support;
    int check_slave_delay;
    int is_reduce_conns;
    int is_reset_conn_enabled;
    int long_query_time;
    int xa_log_detailed;
    int cetus_max_allowed_packet;
    int default_query_cache_timeout;
    int query_cache_enabled;
    int disable_dns_cache;
    double slave_delay_down_threshold_sec;
    double slave_delay_recover_threshold_sec;

    guint invoke_dbg_on_crash;
    /* the --keepalive option isn't available on Unix */
    guint auto_restart;
    gint max_files_number;

    gchar *user;

    gchar *base_dir;
    gchar *conf_dir;

    gchar *default_file;
    GKeyFile *keyfile;

    chassis_plugin *p;
    GOptionEntry *config_entries;

    gchar *pid_file;

    gchar *plugin_dir;
    gchar **plugin_names;

    gchar *log_level;
    gchar *log_filename;
    gchar *log_xa_filename;
    char *default_username;
    char *default_charset;
    char *default_db;

    char *remote_config_url;
} chassis_frontend_t;

/**
 * create a new the frontend for the chassis
 */
chassis_frontend_t *chassis_frontend_new(void) {
    chassis_frontend_t *frontend;

    frontend = g_slice_new0(chassis_frontend_t);
    frontend->max_files_number = 0;
    frontend->disable_threads = 0;
    frontend->is_back_compressed = 0;
    frontend->is_client_compress_support = 0;
    frontend->xa_log_detailed = 0;

    frontend->default_pool_size = 100;
    frontend->max_resp_len = 10 * 1024 * 1024; /* 10M */
    frontend->merged_output_size = 8192;
    frontend->max_header_size = 65536;
    frontend->config_port = 3306;

    frontend->slave_delay_down_threshold_sec = 60.0;
    frontend->default_query_cache_timeout = 100;
    frontend->long_query_time = MAX_QUERY_TIME;
    frontend->cetus_max_allowed_packet = MAX_ALLOWED_PACKET_DEFAULT;
    frontend->disable_dns_cache = 0;
    return frontend;
}

/**
 * free the frontend of the chassis
 */
void chassis_frontend_free(chassis_frontend_t *frontend) {
    if (!frontend) return;

    if (frontend->keyfile) g_key_file_free(frontend->keyfile);
    g_free(frontend->default_file);
    g_free(frontend->log_xa_filename);
    g_free(frontend->log_filename);

    g_free(frontend->base_dir);
    g_free(frontend->conf_dir);
    g_free(frontend->user);
    g_free(frontend->pid_file);
    g_free(frontend->log_level);
    g_free(frontend->plugin_dir);
    g_free(frontend->default_username);
    g_free(frontend->default_db);
    g_free(frontend->default_charset);

    if (frontend->plugin_names) {
        g_strfreev(frontend->plugin_names);
    }

    g_free(frontend->remote_config_url);

    g_slice_free(chassis_frontend_t, frontend);
}


/**
 * setup the options of the chassis
 */
int chassis_frontend_set_chassis_options(chassis_frontend_t *frontend, chassis_options_t *opts) {
    chassis_options_add(opts,
            "verbose-shutdown",
            0, 0, OPTION_ARG_NONE, &(frontend->verbose_shutdown),
            "Always log the exit code when shutting down", NULL);

    chassis_options_add(opts,
            "daemon",
            0, 0, OPTION_ARG_NONE, &(frontend->daemon_mode),
            "Start in daemon-mode", NULL);

    chassis_options_add(opts,
            "user",
            0, 0, OPTION_ARG_STRING, &(frontend->user),
            "Run cetus as user", "<user>");

    chassis_options_add(opts,
            "basedir",
            0, 0, OPTION_ARG_STRING, &(frontend->base_dir),
            "Base directory to prepend to relative paths in the config",
            "<absolute path>");

    chassis_options_add(opts,
            "conf-dir",
            0, 0, OPTION_ARG_STRING, &(frontend->conf_dir),
            "Configuration directory",
            "<absolute path>");

    chassis_options_add(opts,
            "pid-file",
            0, 0, OPTION_ARG_STRING, &(frontend->pid_file),
            "PID file in case we are started as daemon", "<file>");

    chassis_options_add(opts,
            "plugin-dir",
            0, 0, OPTION_ARG_STRING, &(frontend->plugin_dir),
            "Path to the plugins", "<path>");

    chassis_options_add(opts,
            "plugins",
            0, 0, OPTION_ARG_STRING_ARRAY, &(frontend->plugin_names),
            "Plugins to load", "<name>");

    chassis_options_add(opts,
            "log-level",
            0, 0, OPTION_ARG_STRING, &(frontend->log_level),
            "Log all messages of level ... or higher",
            "(error|warning|info|message|debug)");

    chassis_options_add(opts,
            "log-file",
            0, 0, OPTION_ARG_STRING, &(frontend->log_filename),
            "Log all messages in a file", "<file>");

    chassis_options_add(opts,
            "log-xa-file",
            0, 0, OPTION_ARG_STRING, &(frontend->log_xa_filename),
            "Log all xa messages in a file", "<file>");

    chassis_options_add(opts,
            "log-backtrace-on-crash",
            0, 0, OPTION_ARG_NONE, &(frontend->invoke_dbg_on_crash),
            "Try to invoke debugger on crash", NULL);

    chassis_options_add(opts,
            "keepalive",
            0, 0, OPTION_ARG_NONE, &(frontend->auto_restart),
            "Try to restart the proxy if it crashed", NULL);

    chassis_options_add(opts,
            "max-open-files",
            0, 0, OPTION_ARG_INT, &(frontend->max_files_number),
            "Maximum number of open files (ulimit -n)", NULL);

    chassis_options_add(opts,
            "default-charset",
            0, 0, OPTION_ARG_STRING, &(frontend->default_charset),
            "Set the default character set for backends", "<string>");

    chassis_options_add(opts,
            "default-username",
            0, 0, OPTION_ARG_STRING, &(frontend->default_username),
            "Set the default username for visiting backends", "<string>");

    chassis_options_add(opts,
            "default-db",
            0, 0, OPTION_ARG_STRING, &(frontend->default_db),
            "Set the default db for visiting backends", "<string>");

    chassis_options_add(opts,
            "default-pool-size",
            0, 0, OPTION_ARG_INT, &(frontend->default_pool_size),
            "Set the default pool szie for visiting backends", "<integer>");

    chassis_options_add(opts,
            "max-pool-size",
            0, 0, OPTION_ARG_INT, &(frontend->max_pool_size),
            "Set the max pool szie for visiting backends", "<integer>");

    chassis_options_add(opts,
            "max-resp-size",
            0, 0, OPTION_ARG_INT, &(frontend->max_resp_len),
            "Set the max response size for one backend", "<integer>");

    chassis_options_add(opts,
            "merged-output-size",
            0, 0, OPTION_ARG_INT, &(frontend->merged_output_size),
            "set the merged output size for tcp streaming", "<integer>");

    chassis_options_add(opts,
            "max-header-size",
            0, 0, OPTION_ARG_INT, &(frontend->max_header_size),
            "set the max header size for tcp streaming", "<integer>");

    chassis_options_add(opts,
            "worker_id",
            0, 0, OPTION_ARG_INT, &(frontend->worker_id),
            "Set the worker id and the maximum value allowed is 63 and the min value is 1",
            "<integer>");

    chassis_options_add(opts,
            "disable-threads",
            0, 0, OPTION_ARG_NONE, &(frontend->disable_threads),
            "Disable all threads creation", NULL);

    chassis_options_add(opts,
            "enable-back-compress",
            0, 0, OPTION_ARG_NONE, &(frontend->is_back_compressed),
            "enable compression for backend interactions", NULL);

    chassis_options_add(opts,
            "enable-client-compress",
            0, 0, OPTION_ARG_NONE, &(frontend->is_client_compress_support),
            "enable compression for client interactions", NULL);

    chassis_options_add(opts,
            "check-slave-delay",
            0, 0, OPTION_ARG_NONE, &(frontend->check_slave_delay),
            "Check ro backends with heartbeat", NULL);

    chassis_options_add(opts,
            "slave-delay-down",
            0, 0, OPTION_ARG_DOUBLE, &(frontend->slave_delay_down_threshold_sec),
            "Slave will be set down after reach this delay secondes", "<double>");

    chassis_options_add(opts,
            "slave-delay-recover",
            0, 0, OPTION_ARG_DOUBLE, &(frontend->slave_delay_recover_threshold_sec),
            "Slave will recover after below this delay secondes", "<double>");

    chassis_options_add(opts,
            "default-query-cache-timeout",
            0, 0, OPTION_ARG_INT, &(frontend->default_query_cache_timeout),
            "timeout when proxy connect to backends", "<integer>");

    chassis_options_add(opts,
            "long-query-time",
            0, 0, OPTION_ARG_INT, &(frontend->long_query_time),
            "Long query time in ms", "<integer>");

    chassis_options_add(opts,
            "enable-client-found-rows",
            0, 0, OPTION_ARG_NONE, &(frontend->set_client_found_rows),
            "Set client found rows flag", NULL);

    chassis_options_add(opts,
            "reduce-connections",
            0, 0, OPTION_ARG_NONE, &(frontend->is_reduce_conns),
            "Reduce connections when idle connection num is too high", NULL);

    chassis_options_add(opts,
            "enable-reset-connection",
            0, 0, OPTION_ARG_NONE, &(frontend->is_reset_conn_enabled),
            "Restart connections when feature changed", NULL);

    chassis_options_add(opts,
            "enable-query-cache",
            0, 0, OPTION_ARG_NONE, &(frontend->query_cache_enabled),
            "", NULL);

    chassis_options_add(opts,
            "enable-tcp-stream",
            0, 0, OPTION_ARG_NONE, &(frontend->is_tcp_stream_enabled),
            "", NULL);

    chassis_options_add(opts,
            "log-xa-in-detail",
            0, 0, OPTION_ARG_NONE, &(frontend->xa_log_detailed),
            "log xa in detail", NULL);

    chassis_options_add(opts,
            "disable-dns-cache",
            0, 0, OPTION_ARG_NONE, &(frontend->disable_dns_cache),
            "Every new connection to backends will resolve domain name", NULL);

    chassis_options_add(opts,
            "master-preferred",
            0, 0, OPTION_ARG_NONE, &(frontend->master_preferred),
            "Access to master preferentially", NULL);
    chassis_options_add(opts,
            "max-allowed-packet",
            0, 0, OPTION_ARG_INT, &(frontend->cetus_max_allowed_packet),
            "Max allowed packet as in mysql", "<int>");
    chassis_options_add(opts,
            "remote-conf-url",
            0, 0, OPTION_ARG_STRING, &(frontend->remote_config_url),
            "Remote config url, mysql://xx", "<string>");

    return 0;
}


#ifdef HAVE_SIGACTION
static void
log_backtrace(void)
{
    void *array[16];
    int  size, i;
    char **strings;

    size = backtrace(array, 16);
    strings = backtrace_symbols(array, size);
    g_warning("Obtained %d stack frames.", size);

    for (i = 0; i < size; i++) {
        g_warning("%s", strings[i]);
    }

    free (strings);
}

static void sigsegv_handler(int G_GNUC_UNUSED signum) {
    log_backtrace();

    abort(); /* trigger a SIGABRT instead of just exiting */
}
#endif

static gboolean check_plugin_mode_valid(chassis_frontend_t *frontend, chassis *srv)
{
    int i, proxy_mode = 0, sharding_mode = 0;

    for (i = 0; frontend->plugin_names[i]; ++i) {
        char *name = frontend->plugin_names[i];
        if (strcmp(name, "shard") == 0) {
            sharding_mode = 1;
            g_message("set sharding mode true");
        }
        if (strcmp(name, "proxy") == 0) {
            proxy_mode = 1;
        }
    }
    
    if (sharding_mode && proxy_mode) {
        g_critical("shard & proxy is mutual exclusive");
        return FALSE;
    }

    return TRUE;
}

static void g_query_cache_item_free(gpointer q) {
    query_cache_item *item = q;
    network_queue_free(item->queue);
    g_free(item);
}

/* strdup with 1) default value & 2) NULL check */
#define DUP_STRING(STR, DEFAULT) \
        (STR) ? g_strdup(STR) : ((DEFAULT) ? g_strdup(DEFAULT) : NULL)

static void init_parameters(chassis_frontend_t *frontend, chassis *srv)
{
    srv->default_username = DUP_STRING(frontend->default_username, NULL);
    srv->default_charset = DUP_STRING(frontend->default_charset, NULL);
    srv->default_db = DUP_STRING(frontend->default_db, NULL);

    srv->mid_idle_connections = frontend->default_pool_size;
    g_message("set default pool size:%d", srv->mid_idle_connections);

    if (frontend->max_pool_size >= srv->mid_idle_connections) {
        srv->max_idle_connections = frontend->max_pool_size;
    } else {
        srv->max_idle_connections = srv->mid_idle_connections << 1;
    }
    g_message("set max pool size:%d", srv->max_idle_connections);

    srv->max_resp_len = frontend->max_resp_len;
    g_message("set max resp len:%d", srv->max_resp_len);

    srv->merged_output_size = frontend->merged_output_size;
    srv->compressed_merged_output_size = srv->merged_output_size << 3;
    g_message("%s:set merged output size:%d", G_STRLOC, srv->merged_output_size);

    srv->max_header_size = frontend->max_header_size;
    g_message("%s:set max header size:%d", G_STRLOC, srv->max_header_size);

    if (frontend->worker_id > 0) {
        srv->guid_state.worker_id = frontend->worker_id & 0x3f;
    }

#undef DUP_STRING

    srv->client_found_rows = frontend->set_client_found_rows;
    g_message("set client_found_rows %s", srv->client_found_rows?"true":"false");

    srv->xa_log_detailed = frontend->xa_log_detailed;
    if (srv->xa_log_detailed) {
        g_message("%s:xa_log_detailed true", G_STRLOC);
    } else {
        g_message("%s:xa_log_detailed false", G_STRLOC);
    }
    srv->is_reset_conn_enabled = frontend->is_reset_conn_enabled;
    srv->query_cache_enabled = frontend->query_cache_enabled;
    if (srv->query_cache_enabled) {
        srv->query_cache_table = g_hash_table_new_full(g_str_hash,
                g_str_equal, g_free, g_query_cache_item_free);
        srv->cache_index = g_queue_new();
    }
    srv->is_tcp_stream_enabled = frontend->is_tcp_stream_enabled;
    if (srv->is_tcp_stream_enabled) {
        g_message("%s:tcp stream enabled", G_STRLOC);
    }
    srv->disable_threads = frontend->disable_threads;
    srv->is_back_compressed = frontend->is_back_compressed;
    srv->compress_support = frontend->is_client_compress_support;
    srv->check_slave_delay = frontend->check_slave_delay;
    srv->slave_delay_down_threshold_sec = frontend->slave_delay_down_threshold_sec;
    srv->master_preferred = frontend->master_preferred;
    srv->disable_dns_cache = frontend->disable_dns_cache;
    if (frontend->slave_delay_recover_threshold_sec > 0) {
        srv->slave_delay_recover_threshold_sec = frontend->slave_delay_recover_threshold_sec;
        if (frontend->slave_delay_recover_threshold_sec > srv->slave_delay_down_threshold_sec) {
            srv->slave_delay_recover_threshold_sec = srv->slave_delay_down_threshold_sec;
            g_warning("`slave-delay-recover` should be lower than `slave-delay-down`.");
            g_warning("Set slave-delay-recover=%.3f", srv->slave_delay_down_threshold_sec);
        }
    } else {
        srv->slave_delay_recover_threshold_sec = srv->slave_delay_down_threshold_sec / 2;
    }

    srv->default_query_cache_timeout = MAX(frontend->default_query_cache_timeout, 1);
    srv->long_query_time = MIN(frontend->long_query_time, MAX_QUERY_TIME);
    srv->cetus_max_allowed_packet = CLAMP(frontend->cetus_max_allowed_packet,
            MAX_ALLOWED_PACKET_FLOOR, MAX_ALLOWED_PACKET_CEIL);
}


static void 
release_resouces_when_exit(chassis_frontend_t *frontend, chassis *srv, GError *gerr,
        chassis_options_t *opts, chassis_log *log)
{
    if (gerr) g_error_free(gerr);
    if (srv) chassis_free(srv);
    g_debug("%s: call chassis_options_free", G_STRLOC);
    if (opts) chassis_options_free(opts);
    g_debug("%s: call g_hash_table_destroy", G_STRLOC);
    g_debug("%s: call chassis_log_free", G_STRLOC);
    chassis_log_free(log);
    tc_log_end();

    chassis_frontend_free(frontend);
}

static void
resolve_path(chassis *srv, chassis_frontend_t *frontend)
{
    /*
     * these are used before we gathered all the options
     * from the plugins, thus we need to fix them up before
     * dealing with all the rest.
     */
    char *new_path;
    new_path = chassis_resolve_path(srv->base_dir, frontend->log_filename);
    if (new_path && new_path != frontend->log_filename) {
        g_free(frontend->log_filename);
        frontend->log_filename = new_path;
    }

    new_path = chassis_resolve_path(srv->base_dir, frontend->pid_file);
    if (new_path && new_path != frontend->pid_file) {
        g_free(frontend->pid_file);
        frontend->pid_file = new_path;
    }

    new_path = chassis_resolve_path(srv->base_dir, frontend->plugin_dir);
    if (new_path && new_path != frontend->plugin_dir) {
        g_free(frontend->plugin_dir);
        frontend->plugin_dir = new_path;
    }

    new_path = chassis_resolve_path(srv->base_dir, frontend->conf_dir);
    if (new_path && new_path != frontend->conf_dir) {
        g_free(frontend->conf_dir);
        frontend->conf_dir = new_path;
    }
}

static void slow_query_log_handler(const gchar *log_domain, GLogLevelFlags log_level,
        const gchar *message, gpointer user_data)
{
    FILE* fp = user_data;
    time_t t = time(0);
    struct tm *tm = localtime(&t);
    char timestr[32] = {0};
    const int len = 20;
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S ", tm);
    fwrite(timestr, 1, len, fp);
    fwrite(message, 1, strlen(message), fp);
    fwrite("\n", 1, 1, fp);
}

static FILE* init_slow_query_log(const char* main_log)
{
    if (!main_log) {
        return NULL;
    }
    GString* log_name = g_string_new(main_log);
    g_string_append(log_name, ".slowquery.log");

    FILE* fp = fopen(log_name->str, "a");
    if (fp) {
        g_log_set_handler("slowquery", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL
                   | G_LOG_FLAG_RECURSION, slow_query_log_handler, fp);
    }
    g_string_free(log_name, TRUE);
    return fp;
}

/**
 * This is the "real" main which is called on UNIX platforms.
 */
int main_cmdline(int argc, char **argv) {
    chassis *srv = NULL;
#ifdef HAVE_SIGACTION
    static struct sigaction sigsegv_sa;
#endif
    /* read the command-line options */
    chassis_frontend_t *frontend = NULL;
    chassis_options_t *opts = NULL;

    GError *gerr = NULL;
    chassis_log *log = NULL;

    /*
     * a little helper macro to set the src-location that
     * we stepped out at to exit
     */
#define GOTO_EXIT(status) \
    exit_code = status; \
    exit_location = G_STRLOC; \
    goto exit_nicely;

    int exit_code = EXIT_SUCCESS;
    const gchar *exit_location = G_STRLOC;

    /* init module, ... system */
    if (chassis_frontend_init_glib()) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    /* start the logging ... to stderr */
    log = chassis_log_new();
    /* display messages while parsing or loading plugins */
    log->min_lvl = G_LOG_LEVEL_MESSAGE;
    g_log_set_default_handler(chassis_log_func, log);

    /* may fail on library mismatch */
    if (NULL == (srv = chassis_new())) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    /* we need the log structure for the log-rotation */
    srv->log = log;

    frontend = chassis_frontend_new();

    if (frontend == NULL) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    /**
     * parse once to get the basic options like --default-file and --version
     *
     * leave the unknown options in the list
     */
    if (chassis_frontend_init_base_options(&argc, &argv,
                &(frontend->print_version),
                &(frontend->default_file),
                &gerr))
    {
        g_critical("%s: %s", G_STRLOC, gerr->message);
        g_clear_error(&gerr);

        GOTO_EXIT(EXIT_FAILURE);
    }

    if (frontend->default_file) {
        if (!(frontend->keyfile =
              chassis_frontend_open_config_file(frontend->default_file, &gerr)))
        {
            g_critical("%s: loading config from '%s' failed: %s",
                    G_STRLOC,
                    frontend->default_file,
                    gerr->message);
            g_clear_error(&gerr);
            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    /* print the main version number here, but don't exit
     * we check for print_version again, after loading the plugins (if any)
     * and print their version numbers, too. then we exit cleanly.
     */
    if (frontend->print_version) {
        chassis_frontend_print_version();
    }

    /* add the other options which can also appear in the config file */
    opts = chassis_options_new();
    opts->ignore_unknown = TRUE;
    srv->options = opts;

    chassis_frontend_set_chassis_options(frontend, opts);

    if (FALSE == chassis_options_parse_cmdline(opts, &argc, &argv, &gerr)) {
        g_critical("%s", gerr->message);
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (frontend->keyfile) {
        if (FALSE == chassis_keyfile_to_options_with_error(frontend->keyfile,
                    "cetus", opts->options, &gerr))
        {
            g_critical("%s", gerr->message);
            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    if (frontend->remote_config_url) {
        srv->config_manager = chassis_config_from_url(frontend->remote_config_url);
        if (!srv->config_manager) {
            g_critical("remote config init error");
            GOTO_EXIT(EXIT_FAILURE);
        }
        if (!chassis_config_parse_options(srv->config_manager, opts->options)) {
            g_critical("remote_config parse error");
            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    if (chassis_frontend_init_basedir(argv[0], &(frontend->base_dir))) {
        GOTO_EXIT(EXIT_FAILURE);
    }

#ifdef HAVE_SIGACTION
    /* register the sigsegv interceptor */

    memset(&sigsegv_sa, 0, sizeof(sigsegv_sa));
    sigsegv_sa.sa_handler = sigsegv_handler;
    sigemptyset(&sigsegv_sa.sa_mask);

    frontend->invoke_dbg_on_crash = 1;
    if (frontend->invoke_dbg_on_crash && !(RUNNING_ON_VALGRIND)) {
        sigaction(SIGSEGV, &sigsegv_sa, NULL);
    }
#endif

    /*
     * some plugins cannot see the chassis struct from the point
     * where they open files, hence we must make it available
     */
    srv->base_dir = g_strdup(frontend->base_dir);
    srv->plugin_dir = g_strdup(frontend->plugin_dir);
    chassis_frontend_init_plugin_dir(&frontend->plugin_dir, srv->base_dir);

    if (!frontend->conf_dir)
        frontend->conf_dir = g_strdup("conf");

    resolve_path(srv, frontend);

    srv->conf_dir = g_strdup(frontend->conf_dir);

    if (!srv->config_manager) { /* if no remote-config-url, we use local config */
        srv->config_manager =
            chassis_config_from_local_dir(srv->conf_dir, frontend->default_file);
    }

    /*
     * start the logging
     */
    if (frontend->log_filename) {
        log->log_filename = g_strdup(frontend->log_filename);
    }

    if (log->log_filename && FALSE == chassis_log_open(log)) {
        g_critical("can't open log-file '%s': %s", log->log_filename, g_strerror(errno));

        GOTO_EXIT(EXIT_FAILURE);
    }
    FILE* slow_query_log_fp = init_slow_query_log(log->log_filename);
    if (!slow_query_log_fp) {
        g_warning("cannot open slow-query log");
    }

    /*
     * handle log-level after the config-file is read,
     * just in case it is specified in the file
     */
    if (frontend->log_level) {
        if (0 != chassis_log_set_level(log, frontend->log_level)) {
            g_critical("--log-level=... failed, level '%s' is unknown ",
                    frontend->log_level);

            GOTO_EXIT(EXIT_FAILURE);
        }
    } else {
        /* if it is not set, use "critical" as default */
        log->min_lvl = G_LOG_LEVEL_CRITICAL;
    }
    g_message("starting " PACKAGE_STRING);
#ifdef CHASSIS_BUILD_TAG
    g_message("build revision: " CHASSIS_BUILD_TAG);
#endif

    g_message("glib version: %d.%d.%d",
            GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    g_message("libevent version: %s", event_get_version());
    g_message("config dir: %s", frontend->conf_dir);

    if (network_mysqld_init(srv) == -1) {
        g_print("network_mysqld_init failed\n");
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (!frontend->plugin_names) {
        frontend->plugin_names = g_new(char *, 2);
#ifdef DEFAULT_PLUGIN
        frontend->plugin_names[0] = g_strdup(DEFAULT_PLUGIN);
#else
        frontend->plugin_names[0] = g_strdup("proxy");
#endif
        frontend->plugin_names[1] = NULL;
    }

    if (chassis_frontend_load_plugins(srv->modules,
                frontend->plugin_dir,
                frontend->plugin_names)) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (chassis_frontend_init_plugins(srv->modules,
                opts,
                srv->config_manager,
                &argc, &argv,
                frontend->keyfile,
                "cetus",
                &gerr)) {
        g_critical("%s: %s",
                G_STRLOC,
                gerr->message);
        g_clear_error(&gerr);

        GOTO_EXIT(EXIT_FAILURE);
    }


    /* if we only print the version numbers, exit and don't do any more work */
    if (frontend->print_version) {
        chassis_frontend_print_plugin_versions(srv->modules);
        GOTO_EXIT(EXIT_SUCCESS);
    }

    /* we know about the options now, lets parse them */
    opts->ignore_unknown = FALSE;
    opts->help_enabled = TRUE;

    /* handle unknown options */
    if (FALSE == chassis_options_parse_cmdline(opts, &argc, &argv, &gerr)) {
        if (gerr->domain == G_OPTION_ERROR &&
                gerr->code == G_OPTION_ERROR_UNKNOWN_OPTION) {
            g_critical("%s: %s (use --help to show all options)",
                    G_STRLOC,
                    gerr->message);
        } else {
            g_critical("%s: %s (code = %d, domain = %s)",
                    G_STRLOC,
                    gerr->message,
                    gerr->code,
                    g_quark_to_string(gerr->domain)
                    );
        }

        GOTO_EXIT(EXIT_FAILURE);
    }

    /* after parsing the options we should only have the program name left */
    if (argc > 1) {
        g_critical("unknown option: %s", argv[1]);

        GOTO_EXIT(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);

    if (frontend->daemon_mode) {
        chassis_unix_daemonize();
    }

    if (frontend->auto_restart) {
        int child_exit_status = EXIT_SUCCESS; /* forward the exit-status of the child */
        int ret = chassis_unix_proc_keepalive(&child_exit_status);

        if (ret > 0) {
            /* the agent stopped */

            exit_code = child_exit_status;
            goto exit_nicely;
        } else if (ret < 0) {
            GOTO_EXIT(EXIT_FAILURE);
        } else {
            /* we are the child, go on */
        }
    }
    if (frontend->pid_file) {
        if (0 != chassis_frontend_write_pidfile(frontend->pid_file, &gerr)) {
            g_critical("%s", gerr->message);
            g_clear_error(&gerr);

            GOTO_EXIT(EXIT_FAILURE);
        }
    }

    /*
     * log the versions of all loaded plugins
     */
    chassis_frontend_log_plugin_versions(srv->modules);

    /*
     * we have to drop root privileges in chassis_mainloop() after
     * the plugins opened the ports, so we need the user there
     */
    srv->user = g_strdup(frontend->user);

    if (check_plugin_mode_valid(frontend, srv) == FALSE) {
        GOTO_EXIT(EXIT_FAILURE);
    }

    if (frontend->default_username == NULL) {
        g_critical("proxy needs default username");
        GOTO_EXIT(EXIT_FAILURE);
    }

    init_parameters(frontend, srv);

#ifndef SIMPLE_PARSER
    if (!frontend->log_xa_filename)
        frontend->log_xa_filename = g_strdup("logs/xa.log");

    char *new_path = chassis_resolve_path(srv->base_dir, frontend->log_xa_filename);
    if (new_path && new_path != frontend->log_xa_filename) {
        g_free(frontend->log_xa_filename);
        frontend->log_xa_filename = new_path;
    }

    g_message("XA log file: %s", frontend->log_xa_filename);

    if (tc_log_init(frontend->log_xa_filename) == -1) {
        GOTO_EXIT(EXIT_FAILURE);
    }
#endif

    if (frontend->max_files_number) {
        if (0 != chassis_fdlimit_set(frontend->max_files_number)) {
            g_critical("%s: setting fdlimit = %d failed: %s (%d)",
                    G_STRLOC,
                    frontend->max_files_number,
                    g_strerror(errno),
                    errno);
            GOTO_EXIT(EXIT_FAILURE);
        }
    }
    g_debug("max open file-descriptors = %"G_GINT64_FORMAT,
            chassis_fdlimit_get());


    cetus_monitor_start_thread(srv->priv->monitor, srv);

    if (chassis_mainloop(srv)) {
        /* looks like we failed */
        g_critical("%s: Failure from chassis_mainloop. Shutting down.", G_STRLOC);
        GOTO_EXIT(EXIT_FAILURE);
    }

    cetus_monitor_stop_thread(srv->priv->monitor);

exit_nicely:
    /* necessary to set the shutdown flag, because the monitor will continue
     * to schedule timers otherwise, causing an infinite loop in cleanup
     */
    if (!exit_code) {
        exit_location = G_STRLOC;
    }

    struct mallinfo m = mallinfo();

    g_message("Total allocated space (bytes): %d", m.uordblks);
    g_message("Total free space (bytes): %d", m.fordblks);
    g_message("Top-most, releasable space (bytes): %d", m.keepcost);
   
    chassis_set_shutdown_location(exit_location);

    if (frontend && !frontend->print_version) {
        /* add a tag to the logfile */
        g_log(G_LOG_DOMAIN,
                (frontend->verbose_shutdown ? G_LOG_LEVEL_CRITICAL : G_LOG_LEVEL_MESSAGE),
                "shutting down normally, exit code is: %d", exit_code);
    }

#ifdef HAVE_SIGACTION
    /* reset the handler */
    sigsegv_sa.sa_handler = SIG_DFL;
    if (frontend && frontend->invoke_dbg_on_crash && !(RUNNING_ON_VALGRIND)) {
        sigaction(SIGSEGV, &sigsegv_sa, NULL);
    }
#endif

    release_resouces_when_exit(frontend, srv, gerr, opts, log);
    if (slow_query_log_fp) fclose(slow_query_log_fp);

    return exit_code;
}

int main(int argc, char **argv) {
    return main_cmdline(argc, argv);
}


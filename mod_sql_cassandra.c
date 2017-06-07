/*
 * ProFTPD: mod_sql_cassandra -- Support for connecting to Cassandra databases
 * Copyright (c) 2017 TJ Saunders
 *  
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders gives permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 *
 * -----DO NOT EDIT-----
 * $Libraries: -lcassandra$
 */

#define MOD_SQL_CASSANDRA_VERSION		"mod_sql_cassandra/0.0"

#include "conf.h"
#include "privs.h"
#include "mod_sql.h"

#include <cassandra.h>

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030602
# error "ProFTPD 1.3.6rc2 or later required"
#endif

module sql_cassandra_module;

typedef struct db_conn_struct {
  const char *seeds;
  const char *keyspace;
  const char *user;
  const char *pass;
  const CassCluster *cluster;
  const CassSession *session;
  CassTimestampGen *timestamps;

  /* For configuring the SSL/TLS session to Cassandra. */
  const char *ssl_cert_file;
  const char *ssl_key_file;
  const char *ssl_ca_file;

  /* Cassandra version. */
  int cluster_major_version;
  int cluster_minor_version;
  int cluster_patch_version;

} db_conn_t;

typedef struct conn_entry_struct {
  char *name;
  void *data;

  /* Timer handling */
  int timer;
  int ttl;

  /* Connection handling */
  unsigned int nconn;

} conn_entry_t;

#define CASSANDRA_CONN_POOL_SIZE		10

/* Send TCP keepalive probes after this many seconds of inactivity. */
#define CASSANDRA_TCP_KEEPALIVE_DELAY_SECS	300

/* options */
#define CASSANDRA_OPT_NO_CLIENT_SIDE_TIMESTAMPS		0x0001

static pool *conn_pool = NULL;
static array_header *conn_cache = NULL;
static CassConsistency cassandra_consistency = CASS_CONSISTENCY_LOCAL_QUORUM;
static unsigned long cassandra_opts = 0UL;

#define CASSANDRA_TRACE_LEVEL	12
static const char *trace_channel = "sql.cassandra";

MODRET sql_cassandra_close(cmd_rec *);

static conn_entry_t *cassandra_get_conn(char *name) {
  register unsigned int i = 0;

  if (name == NULL) {
    errno = EINVAL;
    return NULL;
  }

  for (i = 0; i < conn_cache->nelts; i++) {
    conn_entry_t *entry = ((conn_entry_t **) conn_cache->elts)[i];

    if (strcmp(name, entry->name) == 0) {
      return entry;
    }
  }

  errno = ENOENT;
  return NULL;
}

static void *cassandra_add_conn(pool *p, char *name, db_conn_t *conn) {
  conn_entry_t *entry = NULL;

  if (p == NULL ||
      name == NULL ||
      conn == NULL) {
    errno = EINVAL;
    return NULL;
  }

  entry = cassandra_get_conn(name);
  if (entry != NULL) {
    return entry;
  }

  entry = (conn_entry_t *) pcalloc(p, sizeof(conn_entry_t));
  entry->name = name;
  entry->data = conn;

  *((conn_entry_t **) push_array(conn_cache)) = entry;
  return entry;
}

static int cassandra_timer_cb(CALLBACK_FRAME) {
  register unsigned int i = 0;
 
  for (i = 0; i < conn_cache->nelts; i++) {
    conn_entry_t *entry;

    entry = ((conn_entry_t **) conn_cache->elts)[i];
    if ((unsigned long) entry->timer == p2) {
      cmd_rec *cmd = NULL;

      sql_log(DEBUG_INFO, "timer expired for connection '%s'", entry->name);

      cmd = pr_cmd_alloc(conn_pool, 2, entry->name, "1");
      sql_cassandra_close(cmd);
      destroy_pool(cmd->pool);

      entry->timer = 0;
    }
  }

  return 0;
}

static void cassandra_log_cb(const CassLogMessage *msg, void *user_data) {
  const char *log_level;
  int use_trace = TRUE, use_sqllog = FALSE;

  log_level = cass_log_level_string(msg->severity);

  if (strcasecmp(log_level, "TRACE") == 0) {
    /* cpp-driver's TRACE logging is a bit too verbose; only use if the
     * trace level is quite high.
     */
    use_trace = FALSE;

    if (pr_trace_get_level(trace_channel) >= 30) {
      use_trace = TRUE;
    }

  } else if (strcasecmp(log_level, "DEBUG") == 0) {
    use_trace = TRUE;

  } else {
    use_sqllog = TRUE;
  }

  if (use_trace) {
    pr_trace_msg(trace_channel, 5, "%u.%03u [%s] (%s:%d:%s): %s",
      (unsigned int) (msg->time_ms / 1000),
      (unsigned int) (msg->time_ms % 1000), log_level, msg->file, msg->line,
      msg->function, msg->message);
  }

  if (use_sqllog) {
    sql_log(DEBUG_FUNC, "%u.%03u [%s] (%s:%d:%s): %s",
      (unsigned int) (msg->time_ms / 1000),
      (unsigned int) (msg->time_ms % 1000), log_level, msg->file, msg->line,
      msg->function, msg->message);
  }
}

static int cassandra_logging_init(void) {
  cass_log_set_callback(cassandra_log_cb, NULL);
  cass_log_set_level(CASS_LOG_TRACE);
  return 0;
}

static int cassandra_logging_free(void) {
  /* The cass_log_cleanup() function is deprecated; no need to call it. */
  return 0;
}

static char *load_file(pool *p, const char *path, long *len) {
  char *file_data;
  long file_datasz;
  FILE *fh;
  struct stat st;
  size_t nread;

  fh = fopen(path, "rb");
  if (fh == NULL) {
    int xerrno = errno;

    sql_log(DEBUG_FUNC, MOD_SQL_CASSANDRA_VERSION
      ": error opening file '%s': %s", path, strerror(xerrno));

    errno = xerrno;
    return NULL;
  }

  if (fstat(fileno(fh), &st) < 0) {
    int xerrno = errno;

    sql_log(DEBUG_FUNC, MOD_SQL_CASSANDRA_VERSION
      ": error checking file '%s': %s", path, strerror(xerrno));

    (void) fclose(fh);
    errno = xerrno;
    return NULL;
  }

  file_datasz = (long) st.st_size; 
  file_data = palloc(p, file_datasz);

  nread = fread(file_data, sizeof(char), file_datasz, fh);
  if (nread != file_datasz) {
    int xerrno = errno;

    if (ferror(fh)) {
      sql_log(DEBUG_FUNC, MOD_SQL_CASSANDRA_VERSION
        ": error reading file '%s' after %lu bytes: %s", path,
        (unsigned long) nread, strerror(xerrno));
    }

    (void) fclose(fh);
    errno = xerrno;
    return NULL;
  }

  (void) fclose(fh);

  *len = nread;
  return file_data;
}

static int cassandra_ssl_load_ca_cert(pool *p, CassSsl *ssl, const char *path) {
  CassError rc;
  char *cert_data;
  long cert_datasz;
  pool *tmp_pool;

  tmp_pool = make_sub_pool(p);

  cert_data = load_file(tmp_pool, path, &cert_datasz);
  if (cert_data == NULL) {
    int xerrno = errno;

    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  rc = cass_ssl_add_trusted_cert_n(ssl, cert_data, cert_datasz);
  if (rc != CASS_OK) {
    sql_log(DEBUG_FUNC, MOD_SQL_CASSANDRA_VERSION
      ": error loading certificate '%s': %s", path, cass_error_desc(rc));

    destroy_pool(tmp_pool);
    errno = EPERM;
    return -1;
  }

  destroy_pool(tmp_pool);
  return 0;
}

static int cassandra_ssl_load_client_cert(pool *p, CassSsl *ssl,
    const char *path) {
  CassError rc;
  char *cert_data;
  long cert_datasz;
  pool *tmp_pool;

  tmp_pool = make_sub_pool(p);

  cert_data = load_file(tmp_pool, path, &cert_datasz);
  if (cert_data == NULL) {
    int xerrno = errno;

    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  rc = cass_ssl_set_cert_n(ssl, cert_data, cert_datasz);
  if (rc != CASS_OK) {
    sql_log(DEBUG_FUNC, MOD_SQL_CASSANDRA_VERSION
      ": error loading certificate '%s': %s", path, cass_error_desc(rc));

    destroy_pool(tmp_pool);
    errno = EPERM;
    return -1;
  }

  destroy_pool(tmp_pool);
  return 0;
}

static int cassandra_ssl_load_client_key(pool *p, CassSsl *ssl,
    const char *path) {
  CassError rc;
  char *key_data;
  long key_datasz;
  pool *tmp_pool;

  tmp_pool = make_sub_pool(p);

  key_data = load_file(tmp_pool, path, &key_datasz);
  if (key_data == NULL) {
    int xerrno = errno;

    destroy_pool(tmp_pool);
    errno = xerrno;
    return -1;
  }

  rc = cass_ssl_set_private_key_n(ssl, key_data, key_datasz, "", 0);
  if (rc != CASS_OK) {
    sql_log(DEBUG_FUNC, MOD_SQL_CASSANDRA_VERSION
      ": error loading private key '%s': %s", path, cass_error_desc(rc));

    destroy_pool(tmp_pool);
    errno = EPERM;
    return -1;
  }

  destroy_pool(tmp_pool);
  return 0;
}

static int cassandra_cluster_init(pool *p, db_conn_t *conn) {
  CassCluster *cluster;
  CassRetryPolicy *default_policy, *logging_policy;

  cluster = cass_cluster_new();
  cass_cluster_set_contact_points(cluster, conn->seeds);

  /* Ensure that Nagle's algorithm is disabled. */
  cass_cluster_set_tcp_nodelay(cluster, cass_true);

  /* Enable TCP keepalives. */
  cass_cluster_set_tcp_keepalive(cluster, cass_true,
    CASSANDRA_TCP_KEEPALIVE_DELAY_SECS);

  if (!(cassandra_opts & CASSANDRA_OPT_NO_CLIENT_SIDE_TIMESTAMPS)) {
    conn->timestamps = cass_timestamp_gen_monotonic_new();
    cass_cluster_set_timestamp_gen(cluster, conn->timestamps);
  }

  if (conn->user != NULL) {
    cass_cluster_set_credentials(cluster, conn->user, conn->pass);
  }

  if (conn->ssl_ca_file != NULL) {
    int res, init_openssl = TRUE;
    CassSsl *ssl;

    /* If specific other modules are present, they will already initialize
     * OpenSSL properly; otherwise, we have to do it here.
     */
    if (pr_module_get("mod_auth_otp.c") != NULL ||
        pr_module_get("mod_digest.c") != NULL ||
        pr_module_get("mod_sftp.c") != NULL ||
        pr_module_get("mod_sql_passwd.c") != NULL ||
        pr_module_get("mod_tls.c") != NULL) {
      init_openssl = FALSE;
    }

    if (init_openssl) {
      ssl = cass_ssl_new();

    } else {
      ssl = cass_ssl_new_no_lib_init();
    }

    res = cassandra_ssl_load_ca_cert(p, ssl, conn->ssl_ca_file);
    if (res < 0) {
      sql_log(DEBUG_FUNC, "error loading ssl-ca '%s': %s", conn->ssl_ca_file,
        strerror(errno));
    }

    if (conn->ssl_cert_file != NULL) {
      res = cassandra_ssl_load_client_cert(p, ssl, conn->ssl_cert_file);
      if (res < 0) {
        sql_log(DEBUG_FUNC, "error loading ssl-cert '%s': %s",
          conn->ssl_cert_file, strerror(errno));
      }
    }

    if (conn->ssl_key_file != NULL) {
      res = cassandra_ssl_load_client_key(p, ssl, conn->ssl_key_file);
      if (res < 0) {
        sql_log(DEBUG_FUNC, "error loading ssl-key '%s': %s",
          conn->ssl_key_file, strerror(errno));
      }
    }

    cass_ssl_set_verify_flags(ssl, CASS_SSL_VERIFY_PEER_CERT);
    cass_cluster_set_ssl(cluster, ssl);
    cass_ssl_free(ssl);
  }

  /* Use the logging RetryPolicy as a logging wrapper around the default
   * RetryPolicy, for visibility.
   */

  default_policy = cass_retry_policy_default_new();
  logging_policy = cass_retry_policy_logging_new(default_policy);
  cass_cluster_set_retry_policy(cluster, logging_policy);
  cass_retry_policy_free(default_policy);
  cass_retry_policy_free(logging_policy);

  cass_cluster_set_latency_aware_routing(cluster, cass_true);
  cass_cluster_set_token_aware_routing(cluster, cass_true);

  /* XXX
   *  cass_cluster_set_connection_heartbeat_interval(cluster, secs);
   *  cass_cluster_set_connection_idle_timeout(cluster, secs);
   */

  conn->cluster = cluster;
  return 0;
}

static int cassandra_cluster_free(db_conn_t *conn) {
  if (conn->cluster != NULL) {
    cass_cluster_free((CassCluster *) conn->cluster);
    conn->cluster = NULL;
  }

  if (conn->timestamps != NULL) {
    cass_timestamp_gen_free(conn->timestamps);
    conn->timestamps = NULL;
  }

  return 0;
}

static int cassandra_get_cluster_version(pool *p, db_conn_t *conn) {
  const CassSchemaMeta *schema;
  CassVersion version;

  schema = cass_session_get_schema_meta((CassSession *) conn->session);
  version = cass_schema_meta_version(schema);
  sql_log(DEBUG_FUNC, "Cassandra cluster using version %d.%d.%d",
    version.major_version, version.minor_version, version.patch_version);

  conn->cluster_major_version = version.major_version;
  conn->cluster_minor_version = version.minor_version;
  conn->cluster_patch_version = version.patch_version;

  cass_schema_meta_free((CassSchemaMeta *) schema);
  return 0;
}

static int cassandra_session_init(pool *p, db_conn_t *conn) {
  CassSession *sess;
  CassFuture *connect_future;
  CassError rc;

  if (conn->cluster == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (conn->session != NULL) {
    return 0;
  }

  sess = cass_session_new();
  connect_future = cass_session_connect_keyspace(sess, conn->cluster,
    conn->keyspace);
  cass_future_wait(connect_future);
  rc = cass_future_error_code(connect_future);
  if (rc != CASS_OK) {
    const char *error_text;
    size_t error_textlen;

    cass_future_error_message(connect_future, &error_text, &error_textlen);
    sql_log(DEBUG_WARN, MOD_SQL_CASSANDRA_VERSION
      ": error connecting to Cassandra using keyspace %s: %.*s", conn->keyspace,
      (int) error_textlen, error_text);

    cass_future_free(connect_future);
    errno = EPERM;
    return -1;
  }

  cass_future_free(connect_future);
  conn->session = sess;
  cassandra_get_cluster_version(p, conn);

  return 0;
}

static int cassandra_session_free(db_conn_t *conn) {
  if (conn->session != NULL) {
    if (pr_trace_get_level(trace_channel) >= CASSANDRA_TRACE_LEVEL) {
      CassMetrics metrics;

      cass_session_get_metrics(conn->session, &metrics);

      /* Latency/throughput */
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "latency metrics: min/mean/max request latency = %lu/%lu/%lu microsecs",
        (unsigned long) metrics.requests.min,
        (unsigned long) metrics.requests.mean,
        (unsigned long) metrics.requests.max);
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "latency metrics: mean request rate = %lu requests/sec",
        (unsigned long) metrics.requests.mean_rate);

      /* Errors/timeouts */
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "error metrics: connection timeouts = %lu",
        (unsigned long) metrics.errors.connection_timeouts);
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "error metrics: pending request timeouts = %lu",
        (unsigned long) metrics.errors.pending_request_timeouts);
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "error metrics: request timeouts = %lu",
        (unsigned long) metrics.errors.request_timeouts);

      /* Watermark thresholds */
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "watermark metrics: exceeded pending requests = %lu",
        (unsigned long) metrics.stats.exceeded_pending_requests_water_mark);
      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        "watermark metrics: exceeded write bytes = %lu",
        (unsigned long) metrics.stats.exceeded_write_bytes_water_mark);
    }

    cass_session_free((CassSession *) conn->session);
    conn->session = NULL;
  }

  return 0;
}

static int cassandra_log_keyspaces(db_conn_t *conn) {
  const char *text;
  CassStatement *stmt;
  CassFuture *future;
  CassError rc;

  /* Note: The particular query to use varies among the Cassandra versions;
   * this will need to take that version into account.
   */

  text = "SELECT keyspace_name FROM system_schema.keyspaces;";
  stmt = cass_statement_new(text, 0);

  /* A consistency of ONE is required for the system keyspace; we are only
   * talking to one node, so requiring any other consistency is unsupportable.
   */
  cass_statement_set_consistency(stmt, CASS_CONSISTENCY_ONE);

  future = cass_session_execute((CassSession *) conn->session, stmt);
  cass_future_wait(future);

  rc = cass_future_error_code(future);
  if (rc == CASS_OK) {
    const CassResult *rs;
    size_t nrows;
    CassIterator *iter;

    rs = cass_future_get_result(future);
    nrows = cass_result_row_count(rs);
    pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL, "keyspace count: %lu",
      (unsigned long) nrows);

    iter = cass_iterator_from_result(rs);
    while (cass_iterator_next(iter)) {
      const CassRow *row;
      const CassValue *val;
      const char *text = NULL;
      size_t text_len = 0;

      pr_signals_handle();

      row = cass_iterator_get_row(iter);
      val = cass_row_get_column(row, 0);
      cass_value_get_string(val, &text, &text_len);

      pr_trace_msg(trace_channel, CASSANDRA_TRACE_LEVEL,
        " keyspace: %.*s", (int) text_len, text);
    }

    cass_iterator_free(iter);
    cass_result_free(rs);

  } else {
    const char *error_text;
    size_t error_textlen;

    cass_future_error_message(future, &error_text, &error_textlen);
    sql_log(DEBUG_WARN, MOD_SQL_CASSANDRA_VERSION
      ": error executing '%s': %.*s", text, (int) error_textlen, error_text);
  }

  cass_future_free(future);
  cass_statement_free(stmt);
  return 0;
}

/* The result set from handling Cassandra queries is built up by the callback
 * function exec_cb(), and stored here.
 */
static int result_ncols = 0;
static array_header *result_list = NULL;

static int exec_stmt(cmd_rec *cmd, db_conn_t *conn, char *text, char **errstr) {
  CassStatement *stmt;
  CassFuture *future;
  CassError rc;
  const CassResult *rs;
  CassIterator *iter;
  size_t nrows, ncols;

  /* Perform the query.  If it doesn't work, log the error, close the
   * connection, then return the error from the query processing.
   */

  stmt = cass_statement_new(text, 0);
  cass_statement_set_consistency(stmt, cassandra_consistency);

  future = cass_session_execute((CassSession *) conn->session, stmt);
  cass_future_wait(future);

  rc = cass_future_error_code(future);
  if (rc != CASS_OK) {
    const char *error_text;
    size_t error_textlen;

    cass_future_error_message(future, &error_text, &error_textlen);
    *errstr = pstrndup(cmd->pool, error_text, error_textlen);

    sql_log(DEBUG_FUNC, "error executing '%s': %s", text, *errstr);
    cass_future_free(future);
    cass_statement_free(stmt);

    errno = EPERM;
    return -1;
  }

  rs = cass_future_get_result(future);
  nrows = cass_result_row_count(rs);
  ncols = cass_result_column_count(rs);
  pr_trace_msg(trace_channel, 9,
    "executing '%s' resulted in row count %lu, column count %lu", text,
    (unsigned long) nrows, (unsigned long) ncols);

  if (result_list == NULL) {
    result_ncols = ncols;
    result_list = make_array(cmd->tmp_pool, ncols, sizeof(char **));
  }

  iter = cass_iterator_from_result(rs);
  while (cass_iterator_next(iter)) {
    register unsigned int i;
    const CassRow *row;
    char ***result_row;

    pr_signals_handle();

    row = cass_iterator_get_row(iter);
    result_row = push_array(result_list);
    *result_row = pcalloc(cmd->tmp_pool, sizeof(char *) * ncols);

    for (i = 0; i < ncols; i++) {
      const CassValue *val;
      CassValueType val_type;
      char *text = NULL;
      size_t textsz = 0;

      val = cass_row_get_column(row, i);
      val_type = cass_value_type(val);
      switch (val_type) {
        case CASS_VALUE_TYPE_ASCII:
        case CASS_VALUE_TYPE_TEXT:
        case CASS_VALUE_TYPE_VARCHAR:
          cass_value_get_string(val, (const char **) &text, &textsz);
          break;

        case CASS_VALUE_TYPE_BIGINT: {
          cass_int64_t num;
          size_t bufsz;

          cass_value_get_int64(val, &num);
          bufsz = 128;
          text = pcalloc(cmd->tmp_pool, bufsz+1);
          textsz = snprintf(text, bufsz, "%ld", (long int) num);
          break;
        }

        case CASS_VALUE_TYPE_INT: {
          cass_int32_t num;
          size_t bufsz;

          cass_value_get_int32(val, &num);
          bufsz = 128;
          text = pcalloc(cmd->tmp_pool, bufsz+1);
          textsz = snprintf(text, bufsz, "%d", (int) num);
          break;
        }

        case CASS_VALUE_TYPE_SMALL_INT: {
          cass_int16_t num;
          size_t bufsz;

          cass_value_get_int16(val, &num);
          bufsz = 128;
          text = pcalloc(cmd->tmp_pool, bufsz+1);
          textsz = snprintf(text, bufsz, "%d", (short) num);
          break;
        }

        case CASS_VALUE_TYPE_TINY_INT: {
          cass_int8_t num;
          size_t bufsz;

          cass_value_get_int8(val, &num);
          bufsz = 128;
          text = pcalloc(cmd->tmp_pool, bufsz+1);
          textsz = snprintf(text, bufsz, "%c", (char) num);
          break;
        }

        default:
          sql_log(DEBUG_FUNC, "unknown/unsupported Cassandra value type: %d",
            val_type);
      }

      if (text != NULL) {
        (*result_row)[i] = pstrndup(cmd->tmp_pool, text, textsz);

      } else {
        (*result_row)[i] = pstrdup(cmd->tmp_pool, "");
      }
    }
  }

  cass_iterator_free(iter);
  cass_result_free(rs);
  cass_future_free(future);
  cass_statement_free(stmt);

  return 0;
}

static modret_t *cassandra_get_data(cmd_rec *cmd) {
  register unsigned int i;
  unsigned int count, k = 0;
  char **data;
  sql_data_t *sd = pcalloc(cmd->tmp_pool, sizeof(sql_data_t));

  if (result_list == NULL) {
    return mod_create_data(cmd, sd);
  }

  sd->rnum = result_list->nelts;
  sd->fnum = result_ncols;
  count = sd->rnum * sd->fnum;
  data = pcalloc(cmd->tmp_pool, sizeof(char *) * (count + 1));

  for (i = 0; i < result_list->nelts; i++) {
    register int j;
    char **row;

    row = ((char ***) result_list->elts)[i];
    for (j = 0; j < result_ncols; j++) {
      data[k++] = pstrdup(cmd->tmp_pool, row[j]);
    }
  }

  data[k] = NULL;
  sd->data = data;

  /* Reset these variables.  The memory in them is allocated from this
   * same cmd_rec, and will be recovered when the cmd_rec is destroyed.
   */
  result_ncols = 0;
  result_list = NULL;

  return mod_create_data(cmd, sd);
}

MODRET sql_cassandra_open(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  int res;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_open");

  if (cmd->argc < 1) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_open");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }    

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_open");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  } 

  conn = (db_conn_t *) entry->data;

  /* If we're already open (nconn > 0), increment the number of connections.
   * Reset our timer if we have one, and return HANDLED.
   */
  if (entry->nconn > 0) {
    entry->nconn++;

    if (entry->timer) {
      pr_timer_reset(entry->timer, &sql_cassandra_module);
    }

    sql_log(DEBUG_INFO, "'%s' connection count is now %u", entry->name,
      entry->nconn);
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_open");
    return PR_HANDLED(cmd);
  }

  res = cassandra_cluster_init(cmd->tmp_pool, conn);
  if (res < 0) {
    const char *errstr;

    errstr = strerror(errno);
    sql_log(DEBUG_FUNC, "error initializing Cassandra: %s", errstr);
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  res = cassandra_session_init(cmd->tmp_pool, conn);
  if (res < 0) {
    const char *errstr;

    errstr = strerror(errno);

    cassandra_cluster_free(conn);
    sql_log(DEBUG_FUNC, "error connecting to Cassandra: %s", errstr);
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  if (pr_trace_get_level(trace_channel) >= CASSANDRA_TRACE_LEVEL) {
    /* Add some Cassandra information to the logs. */
    cassandra_log_keyspaces(conn);
  }

  entry->nconn++;

  if (pr_sql_conn_policy == SQL_CONN_POLICY_PERSESSION) {
    /* If the connection policy is PERSESSION... */
    if (entry->nconn == 1) {
      /* ...and we are actually opening the first connection to the database;
       * we want to make sure this connection stays open, after this first use
       * (as per Bug#3290).  To do this, we re-bump the connection count.
       */
      entry->nconn++;
    }

  } else if (entry->ttl > 0) {
    /* Set up our timer, if necessary. */
    entry->timer = pr_timer_add(entry->ttl, -1, &sql_cassandra_module,
      cassandra_timer_cb, "cassandra connection ttl");

    sql_log(DEBUG_INFO, "'%s' connection: %d second timer started",
      entry->name, entry->ttl);

    /* Timed connections get re-bumped so they don't go away when
     * sql_cassandra_close() is called.
     */
    entry->nconn++;
  }

  sql_log(DEBUG_INFO, "'%s' connection opened", entry->name);
  sql_log(DEBUG_INFO, "'%s' connection count is now %u", entry->name,
    entry->nconn);
  pr_event_generate("mod_sql.db.connection-opened", &sql_cassandra_module);

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_open");
  return PR_HANDLED(cmd);
}

MODRET sql_cassandra_close(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_close");

  if (cmd->argc < 1 ||
      cmd->argc > 2) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_close");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_close");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  }

  conn = (db_conn_t *) entry->data;

  /* If we're closed already (nconn == 0), return HANDLED. */
  if (entry->nconn == 0) {
    sql_log(DEBUG_INFO, "'%s' connection count is now %u", entry->name,
      entry->nconn);
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_close");
    return PR_HANDLED(cmd);
  }

  /* Decrement nconn. If our count is 0 or we received a second arg, close
   * the connection, explicitly set the counter to 0, and remove any timers.
   */
  if ((--entry->nconn) == 0 ||
      (cmd->argc == 2 && cmd->argv[1])) {

    if (conn->session != NULL) {
      cassandra_session_free(conn);
    }

    entry->nconn = 0;

    if (entry->timer) {
      pr_timer_remove(entry->timer, &sql_cassandra_module);
      entry->timer = 0;
      sql_log(DEBUG_INFO, "'%s' connection timer stopped", entry->name);
    }

    sql_log(DEBUG_INFO, "'%s' connection closed", entry->name);
    pr_event_generate("mod_sql.db.connection-closed", &sql_cassandra_module);
  }

  sql_log(DEBUG_INFO, "'%s' connection count is now %u", entry->name,
    entry->nconn);
  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_close");
  
  return PR_HANDLED(cmd);
}

MODRET sql_cassandra_cleanup(cmd_rec *cmd) {
  destroy_pool(conn_pool);
  conn_pool = NULL;
  conn_cache = NULL;

  return mod_create_data(cmd, NULL);
}

MODRET sql_cassandra_define_conn(cmd_rec *cmd) {
  char *name = NULL, *info = NULL, *ptr;
  const char *ssl_cert_file = NULL, *ssl_key_file = NULL, *ssl_ca_file = NULL;
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL; 

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_defineconnection");

  if (cmd->argc < 4 ||
      cmd->argc > 10 ||
      !cmd->argv[0]) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_defineconnection");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  if (conn_pool == NULL) {
    pr_log_pri(PR_LOG_WARNING, "WARNING: the mod_sql_cassandra module has not "
      "been properly intialized.  Please make sure your --with-modules "
      "configure option lists mod_sql BEFORE mod_sql_cassandra, and recompile");

    sql_log(DEBUG_FUNC, "%s", "The mod_sql_cassandra module has not been "
      "properly intialized.  Please make sure your --with-modules configure "
      "option lists mod_sql BEFORE mod_sql_cassandra, and recompile");
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_defineconnection");

    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "uninitialized module");
  }

  conn = (db_conn_t *) pcalloc(conn_pool, sizeof(db_conn_t));

  name = pstrdup(conn_pool, cmd->argv[0]);
  conn->user = pstrdup(conn_pool, cmd->argv[1]);
  conn->pass = pstrdup(conn_pool, cmd->argv[2]);

  info = pstrdup(conn_pool, cmd->argv[3]);

  ptr = strchr(info, '@');
  if (ptr == NULL) {
    const char *errstr;

    errstr = "missing required keyspace name";
    sql_log(DEBUG_FUNC, "%s", errstr);
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_defineconnection");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  conn->seeds = pstrdup(conn_pool, ptr + 1);
  *ptr = '\0';
  conn->keyspace = info;

  /* SSL parameters, if configured. */
  if (cmd->argc >= 6) {
    ssl_cert_file = cmd->argv[5];
    if (ssl_cert_file != NULL) {
      conn->ssl_cert_file = pstrdup(conn_pool, ssl_cert_file);
    }
  }

  if (cmd->argc >= 7) {
    ssl_key_file = cmd->argv[6];
    if (ssl_key_file != NULL) {
      conn->ssl_key_file = pstrdup(conn_pool, ssl_key_file);
    }
  }

  if (cmd->argc >= 8) {
    ssl_ca_file = cmd->argv[7];
    if (ssl_ca_file != NULL) {
      conn->ssl_ca_file = pstrdup(conn_pool, ssl_ca_file);
    }
  }

  if (cmd->argc >= 9) {
    /* ssl-ca-dir ignored/unsupported */
  }

  if (cmd->argc >= 10) {
    /* ssl-ciphers ignored/unsupported */
  }

  /* Insert the new conn_info into the connection hash */
  entry = cassandra_add_conn(conn_pool, name, (void *) conn);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_defineconnection");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "named connection already exists");
  }

  if (cmd->argc >= 5) {
    entry->ttl = (int) strtol(cmd->argv[4], (char **) NULL, 10);
    if (entry->ttl >= 1) {
      pr_sql_conn_policy = SQL_CONN_POLICY_TIMER;

    } else {
      entry->ttl = 0;
    }
  }

  entry->timer = 0;
  entry->nconn = 0;

  sql_log(DEBUG_INFO, " name: '%s'", entry->name);
  sql_log(DEBUG_INFO, "  dsn: '%s@%s'", conn->keyspace, conn->seeds);
  sql_log(DEBUG_INFO, "  ttl: '%d'", entry->ttl);

  if (conn->ssl_cert_file != NULL) {
    sql_log(DEBUG_INFO, "   ssl: client cert = '%s'", conn->ssl_cert_file);
  }

  if (conn->ssl_key_file != NULL) {
    sql_log(DEBUG_INFO, "   ssl: client key = '%s'", conn->ssl_key_file);
  }

  if (conn->ssl_ca_file != NULL) {
    sql_log(DEBUG_INFO, "   ssl: CA file = '%s'", conn->ssl_ca_file);
  }

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_defineconnection");
  return PR_HANDLED(cmd);
}

MODRET sql_cassandra_exit(cmd_rec *cmd) {
  register unsigned int i = 0;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_exit");

  for (i = 0; i < conn_cache->nelts; i++) {
    conn_entry_t *entry;
    db_conn_t *conn;

    entry = ((conn_entry_t **) conn_cache->elts)[i];
    if (entry->nconn > 0) {
      cmd_rec *tmp;

      tmp = pr_cmd_alloc(conn_pool, 2, entry->name, "1");
      sql_cassandra_close(tmp);
      destroy_pool(tmp->pool);
    }

    conn = (db_conn_t *) entry->data;
    cassandra_cluster_free(conn);
  }

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_exit");
  return PR_HANDLED(cmd);
}

MODRET sql_cassandra_select(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *mr = NULL;
  char *errstr = NULL, *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_select");

  if (cmd->argc < 2) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_select");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_select");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  }
 
  conn = (db_conn_t *) entry->data;

  mr = sql_cassandra_open(cmd);
  if (MODRET_ERROR(mr)) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_select");
    return mr;
  }

  /* Construct the query string. */
  if (cmd->argc == 2) {
    query = pstrcat(cmd->tmp_pool, "SELECT ", cmd->argv[1], NULL);

  } else {
    query = pstrcat(cmd->tmp_pool, cmd->argv[2], " FROM ", cmd->argv[1], NULL);

    if (cmd->argc > 3 && cmd->argv[3])
      query = pstrcat(cmd->tmp_pool, query, " WHERE ", cmd->argv[3], NULL);

    if (cmd->argc > 4 && cmd->argv[4])
      query = pstrcat(cmd->tmp_pool, query, " LIMIT ", cmd->argv[4], NULL);

    if (cmd->argc > 5) {
      register unsigned int i = 0;

      /* Handle the optional arguments -- they're rare, so in this case
       * we'll play with the already constructed query string, but in 
       * general we should probably take optional arguments into account 
       * and put the query string together later once we know what they are.
       */
    
      for (i = 5; i < cmd->argc; i++) {
	if (cmd->argv[i] &&
            strcasecmp("DISTINCT", cmd->argv[i]) == 0)
	  query = pstrcat(cmd->tmp_pool, "DISTINCT ", query, NULL);
      }
    }

    query = pstrcat(cmd->tmp_pool, "SELECT ", query, NULL);
  }

  /* Log the query string */
  sql_log(DEBUG_INFO, "query \"%s\"", query);

  /* Perform the query.  If it doesn't work, log the error, close the
   * connection, then return the error from the query processing.
   */

  if (exec_stmt(cmd, conn, query, &errstr) < 0) {
    close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
    sql_cassandra_close(close_cmd);
    destroy_pool(close_cmd->pool);

    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_select");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  mr = cassandra_get_data(cmd);
  
  /* Close the connection, return the data. */
  close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
  sql_cassandra_close(close_cmd);
  destroy_pool(close_cmd->pool);

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_select");
  return mr;
}

MODRET sql_cassandra_insert(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *mr = NULL;
  char *errstr = NULL, *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_insert");

  if (cmd->argc != 2 &&
      cmd->argc != 4) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_insert");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_insert");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  }

  conn = (db_conn_t *) entry->data;

  mr = sql_cassandra_open(cmd);
  if (MODRET_ERROR(mr)) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_insert");
    return mr;
  }

  /* Construct the query string. */
  if (cmd->argc == 2) {
    query = pstrcat(cmd->tmp_pool, "INSERT ", cmd->argv[1], NULL);

  } else {
    query = pstrcat(cmd->tmp_pool, "INSERT INTO ", cmd->argv[1],
      " (", cmd->argv[2], ") VALUES (", cmd->argv[3], ")", NULL);
  }

  /* Log the query string */
  sql_log(DEBUG_INFO, "query \"%s\"", query);

  /* Perform the query.  If it doesn't work, log the error, close the
   * connection (and log any errors there, too) then return the error
   * from the query processing.
   */

  if (exec_stmt(cmd, conn, query, &errstr) < 0) {
    close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
    sql_cassandra_close(close_cmd);
    destroy_pool(close_cmd->pool);

    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_insert");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  /* Reset these variables.  The memory in them is allocated from this
   * same cmd_rec, and will be recovered when the cmd_rec is destroyed.
   */
  result_ncols = 0;
  result_list = NULL;

  /* Close the connection and return HANDLED. */
  close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
  sql_cassandra_close(close_cmd);
  destroy_pool(close_cmd->pool);

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_insert");
  return PR_HANDLED(cmd);
}

MODRET sql_cassandra_update(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *mr = NULL;
  char *errstr = NULL, *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_update");

  if (cmd->argc < 2 || cmd->argc > 4) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_update");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_update");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  }

  conn = (db_conn_t *) entry->data;

  mr = sql_cassandra_open(cmd);
  if (MODRET_ERROR(mr)) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_update");
    return mr;
  }

  /* Construct the query string. */
  if (cmd->argc == 2) {
    query = pstrcat(cmd->tmp_pool, "UPDATE ", cmd->argv[1], NULL);

  } else {
    query = pstrcat(cmd->tmp_pool, "UPDATE ", cmd->argv[1], " SET ",
      cmd->argv[2], NULL);

    if (cmd->argc > 3 &&
        cmd->argv[3]) {
      query = pstrcat(cmd->tmp_pool, query, " WHERE ", cmd->argv[3], NULL);
    }
  }

  /* Log the query string. */
  sql_log(DEBUG_INFO, "query \"%s\"", query);

  /* Perform the query.  If it doesn't work close the connection, then
   * return the error from the query processing.
   */

  if (exec_stmt(cmd, conn, query, &errstr) < 0) {
    close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
    sql_cassandra_close(close_cmd);
    destroy_pool(close_cmd->pool);

    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_update");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  /* Reset these variables.  The memory in them is allocated from this
   * same cmd_rec, and will be recovered when the cmd_rec is destroyed.
   */
  result_ncols = 0;
  result_list = NULL;

  /* Close the connection, return HANDLED.  */
  close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
  sql_cassandra_close(close_cmd);
  destroy_pool(close_cmd->pool);

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_update");
  return PR_HANDLED(cmd);
}

MODRET sql_cassandra_prepare(cmd_rec *cmd) {
  if (cmd->argc != 1) {
    return PR_ERROR(cmd);
  }

  conn_pool = (pool *) cmd->argv[0];

  if (conn_cache == NULL) {
    conn_cache = make_array(conn_pool, CASSANDRA_CONN_POOL_SIZE,
      sizeof(conn_entry_t *));
  }

  return mod_create_data(cmd, NULL);
}

MODRET sql_cassandra_procedure(cmd_rec *cmd) {
  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_procedure");

  if (cmd->argc != 3) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_procedure");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_procedure");
  return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
    "backend does not support procedures");
}

MODRET sql_cassandra_query(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *mr = NULL;
  char *errstr = NULL, *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_query");

  if (cmd->argc != 2) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_query");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_query");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  }

  conn = (db_conn_t *) entry->data;

  mr = sql_cassandra_open(cmd);
  if (MODRET_ERROR(mr)) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_query");
    return mr;
  }

  query = pstrdup(cmd->tmp_pool, cmd->argv[1]);

  /* Log the query string */
  sql_log(DEBUG_INFO, "query \"%s\"", query);

  /* Perform the query.  If it doesn't work close the connection, then
   * return the error from the query processing.
   */

  if (exec_stmt(cmd, conn, query, &errstr) < 0) {
    close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
    sql_cassandra_close(close_cmd);
    destroy_pool(close_cmd->pool);

    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_query");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, errstr);
  }

  mr = cassandra_get_data(cmd);
  
  /* Close the connection, return the data. */
  close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
  sql_cassandra_close(close_cmd);
  destroy_pool(close_cmd->pool);

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_query");
  return mr;
}

/* Liberally borrowed from MySQL-3.23.55's libmysql.c file,
 * mysql_odbc_escape_string() function.
 */
static void cassandra_escape_string(char *to, const char *from,
    size_t fromlen) {
  const char *end;

  for (end = from + fromlen; from != end; from++) {
    switch (*from) {
      case 0:
        *to++ = '\\';
        *to++ = '0';
        break;

      case '\n':
        *to++ = '\\';
        *to++ = 'n';
        break;

      case '\r':
        *to++ = '\\';
        *to++ = 'r';
        break;

      case '\\':
        *to++ = '\\';
        *to++ = '\\';
        break;

      case '\'':
        *to++ = '\'';
        *to++ = '\'';
        break;

      case '"':
        *to++ = '\\';
        *to++ = '"';
        break;

      case '\032':
        *to++ = '\\';
        *to++ = 'Z';
        break;

       default:
         *to++ = *from;
    }
  }
}

MODRET sql_cassandra_quote(cmd_rec *cmd) {
  conn_entry_t *entry = NULL;
  modret_t *mr = NULL;
  char *escaped = NULL, *unescaped = NULL;
  size_t unescaped_len;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_escapestring");

  if (cmd->argc != 2) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_escapestring");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Get the named connection. */
  entry = cassandra_get_conn(cmd->argv[0]);
  if (entry == NULL) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_escapestring");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
      "unknown named connection");
  }

  /* Make sure the connection is open. */
  mr = sql_cassandra_open(cmd);
  if (MODRET_ERROR(mr)) {
    sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_escapestring");
    return mr;
  }

  unescaped = cmd->argv[1];
  unescaped_len = strlen(unescaped);

  escaped = (char *) pcalloc(cmd->pool, sizeof(char) * (unescaped_len * 2) + 1);
  cassandra_escape_string(escaped, unescaped, unescaped_len);

  close_cmd = pr_cmd_alloc(cmd->tmp_pool, 1, entry->name);
  sql_cassandra_close(close_cmd);
  destroy_pool(close_cmd->pool);

  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_escapestring");
  return mod_create_data(cmd, escaped);
}

MODRET sql_cassandra_checkauth(cmd_rec *cmd) {
  sql_log(DEBUG_FUNC, "%s", "entering \tcassandra cmd_checkauth");

  if (cmd->argc != 3) {
    sql_log(DEBUG_FUNC, "exiting \tcassandra cmd_checkauth");
    return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION, "badly formed request");
  }

  /* Cassandra doesn't provide this functionality. */

  sql_log(DEBUG_WARN, MOD_SQL_CASSANDRA_VERSION
    ": Cassandra does not support the 'Backend' SQLAuthType");
  sql_log(DEBUG_FUNC, "%s", "exiting \tcassandra cmd_checkauth");

  return PR_ERROR_MSG(cmd, MOD_SQL_CASSANDRA_VERSION,
    "Cassandra does not support the 'Backend' SQLAuthType");
}

MODRET sql_cassandra_identify(cmd_rec *cmd) {
  sql_data_t *sd = NULL;

  sd = (sql_data_t *) pcalloc(cmd->tmp_pool, sizeof(sql_data_t));
  sd->data = (char **) pcalloc(cmd->tmp_pool, sizeof(char *) * 2);

  sd->rnum = 1;
  sd->fnum = 2;

  sd->data[0] = MOD_SQL_CASSANDRA_VERSION;
  sd->data[1] = MOD_SQL_API_V1;

  return mod_create_data(cmd, (void *) sd);
}  

static cmdtable sql_cassandra_cmdtable[] = {
  { CMD, "sql_checkauth",	G_NONE, sql_cassandra_checkauth, FALSE, FALSE },
  { CMD, "sql_close",		G_NONE, sql_cassandra_close,	 FALSE, FALSE },
  { CMD, "sql_cleanup",		G_NONE, sql_cassandra_cleanup,	 FALSE, FALSE },
  { CMD, "sql_defineconnection",G_NONE, sql_cassandra_define_conn,FALSE,FALSE },
  { CMD, "sql_escapestring",	G_NONE, sql_cassandra_quote,	 FALSE, FALSE },
  { CMD, "sql_exit",		G_NONE,	sql_cassandra_exit, 	 FALSE, FALSE },
  { CMD, "sql_identify",	G_NONE, sql_cassandra_identify,	 FALSE, FALSE },
  { CMD, "sql_insert",		G_NONE, sql_cassandra_insert,	 FALSE, FALSE },
  { CMD, "sql_open",		G_NONE,	sql_cassandra_open,	 FALSE, FALSE },
  { CMD, "sql_prepare",		G_NONE, sql_cassandra_prepare,	 FALSE, FALSE },
  { CMD, "sql_procedure",	G_NONE, sql_cassandra_procedure, FALSE, FALSE },
  { CMD, "sql_query",		G_NONE, sql_cassandra_query,	 FALSE, FALSE },
  { CMD, "sql_select",		G_NONE, sql_cassandra_select,	 FALSE, FALSE },
  { CMD, "sql_update",		G_NONE, sql_cassandra_update,	 FALSE, FALSE },

  { 0, NULL }
};

/* Event handlers
 */

static void sql_cassandra_mod_load_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_sql_cassandra.c", (const char *) event_data) == 0) {
    cassandra_logging_init();

    /* Register ourselves with mod_sql. */
    if (sql_register_backend("cassandra", sql_cassandra_cmdtable) < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_SQL_CASSANDRA_VERSION
        ": notice: error registering backend: %s", strerror(errno));
      pr_session_end(0);
    }
  }
}

static void sql_cassandra_mod_unload_ev(const void *event_data,
    void *user_data) {
  if (strcmp("mod_sql_cassandra.c", (const char *) event_data) == 0) {
    pr_event_unregister(&sql_cassandra_module, NULL, NULL);
    cassandra_logging_free();

    /* Unregister ourselves with mod_sql. */
    if (sql_unregister_backend("cassandra") < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_SQL_CASSANDRA_VERSION
        ": notice: error unregistering backend: %s", strerror(errno));
      pr_session_end(0);
    }
  }
}

/* Configuration handlers
 */

/* usage: SQLCassandraConsistency consistency */
MODRET set_sqlcassandraconsistency(cmd_rec *cmd) {
  CassConsistency consistency;
  const char *consistency_str;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL);

  consistency_str = cmd->argv[1];

  if (strcasecmp(consistency_str, "All") == 0) {
    consistency = CASS_CONSISTENCY_ALL;

  } else if (strcasecmp(consistency_str, "Any") == 0) {
    consistency = CASS_CONSISTENCY_ANY;

  } else if (strcasecmp(consistency_str, "EachQuorum") == 0 ||
             strcasecmp(consistency_str, "EACH_QUORUM") == 0) {
    consistency = CASS_CONSISTENCY_EACH_QUORUM;

  } else if (strcasecmp(consistency_str, "LocalOne") == 0 ||
             strcasecmp(consistency_str, "LOCAL_ONE") == 0) {
    consistency = CASS_CONSISTENCY_LOCAL_ONE;

  } else if (strcasecmp(consistency_str, "LocalQuorum") == 0 ||
             strcasecmp(consistency_str, "LOCAL_QUORUM") == 0) {
    consistency = CASS_CONSISTENCY_LOCAL_QUORUM;

  } else if (strcasecmp(consistency_str, "One") == 0 ||
             strcasecmp(consistency_str, "1") == 0) {
    consistency = CASS_CONSISTENCY_ONE;

  } else if (strcasecmp(consistency_str, "Quorum") == 0) {
    consistency = CASS_CONSISTENCY_QUORUM;

  } else if (strcasecmp(consistency_str, "Three") == 0 ||
             strcasecmp(consistency_str, "3") == 0) {
    consistency = CASS_CONSISTENCY_THREE;

  } else if (strcasecmp(consistency_str, "Two") == 0 ||
             strcasecmp(consistency_str, "2") == 0) {
    consistency = CASS_CONSISTENCY_TWO;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "unknown/supported Cassandra consistency: ", consistency_str, NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(CassConsistency));
  *((CassConsistency *) c->argv[0]) = consistency;

  return PR_HANDLED(cmd);
}

/* usage: SQLCassandraOptions opt1 opt2 ... */
MODRET set_sqlcassandraoptions(cmd_rec *cmd) {
  config_rec *c = NULL;
  register unsigned int i = 0;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "NoClientSideTimestamps") == 0) {
      opts |= CASSANDRA_OPT_NO_CLIENT_SIDE_TIMESTAMPS;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown SQLCassandraOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  return PR_HANDLED(cmd);
}

/* Initialization routines
 */

static int sql_cassandra_init(void) {

  /* Register listeners for the load and unload events. */
  pr_event_register(&sql_cassandra_module, "core.module-load",
    sql_cassandra_mod_load_ev, NULL);
  pr_event_register(&sql_cassandra_module, "core.module-unload",
    sql_cassandra_mod_unload_ev, NULL);

  if (strlen(CASS_VERSION_SUFFIX) == 0) {
    pr_log_debug(DEBUG3, MOD_SQL_CASSANDRA_VERSION
      ": using datastax-cpp %u.%u.%u", (unsigned int) CASS_VERSION_MAJOR,
      (unsigned int) CASS_VERSION_MINOR, (unsigned int) CASS_VERSION_PATCH);

  } else { 
    pr_log_debug(DEBUG3, MOD_SQL_CASSANDRA_VERSION
      ": using datastax-cpp %u.%u.%u%s", (unsigned int) CASS_VERSION_MAJOR,
      (unsigned int) CASS_VERSION_MINOR, (unsigned int) CASS_VERSION_PATCH,
      CASS_VERSION_SUFFIX);
  }

  return 0;
}

static int sql_cassandra_sess_init(void) {
  config_rec *c;

  if (conn_pool == NULL) {
    conn_pool = make_sub_pool(session.pool);
    pr_pool_tag(conn_pool, "Cassandra connection pool");
  }

  if (conn_cache == NULL) {
    conn_cache = make_array(make_sub_pool(session.pool),
      CASSANDRA_CONN_POOL_SIZE, sizeof(conn_entry_t *));
  }

  c = find_config(main_server->conf, CONF_PARAM, "SQLCassandraConsistency",
    FALSE);
  if (c != NULL) {
    cassandra_consistency = *((CassConsistency *) c->argv[0]);
  }

  pr_trace_msg(trace_channel, 10, "using consistency level %s for statements",
    cass_consistency_string(cassandra_consistency));

  c = find_config(main_server->conf, CONF_PARAM, "SQLCassandraOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    cassandra_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "SQLCassandraOptions", FALSE);
  }

  return 0;
}

/* Module API tables
 */

static conftable sql_cassandra_conftab[] = {
  { "SQLCassandraConsistency",	set_sqlcassandraconsistency,	NULL },
  { "SQLCassandraOptions",	set_sqlcassandraoptions,	NULL },

  { NULL, NULL, NULL }
};

module sql_cassandra_module = {
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "sql_cassandra",

  /* Module configuration directive table */
  sql_cassandra_conftab,

  /* Module command handler table */
  NULL,

  /* Module authentication handler table */
  NULL,

  /* Module initialization */
  sql_cassandra_init,

  /* Session initialization */
  sql_cassandra_sess_init,

  /* Module version */
  MOD_SQL_CASSANDRA_VERSION
};

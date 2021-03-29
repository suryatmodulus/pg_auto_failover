/*
 * src/bin/pg_autoctl/pgsql.h
 *     Functions for interacting with a postgres server
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#ifndef PGSQL_H
#define PGSQL_H


#include <limits.h>
#include <stdbool.h>

#include "libpq-fe.h"
#include "portability/instr_time.h"

#include "defaults.h"
#include "pgsetup.h"
#include "state.h"


/*
 * OID values from PostgreSQL src/include/catalog/pg_type.h
 */
#define BOOLOID 16
#define NAMEOID 19
#define INT4OID 23
#define INT8OID 20
#define TEXTOID 25
#define LSNOID 3220

/*
 * Maximum connection info length as used in walreceiver.h
 */
#define MAXCONNINFO 1024


/*
 * pg_stat_replication.sync_state is one if:
 *   sync, async, quorum, potential
 */
#define PGSR_SYNC_STATE_MAXLENGTH 10

/*
 * We receive a list of "other nodes" from the monitor, and we store that list
 * in local memory. We pre-allocate the memory storage, and limit how many node
 * addresses we can handle because of the pre-allocation strategy.
 */
#define NODE_ARRAY_MAX_COUNT 12


/* abstract representation of a Postgres server that we can connect to */
typedef enum
{
	PGSQL_CONN_LOCAL = 0,
	PGSQL_CONN_MONITOR,
	PGSQL_CONN_COORDINATOR,
	PGSQL_CONN_UPSTREAM,
	PGSQL_CONN_APP
} ConnectionType;


/*
 * Retry policy to follow when we fail to connect to a Postgres URI.
 *
 * In almost all the code base the retry mechanism is implemented in the main
 * loop so we want to fail fast and let the main loop handle the connection
 * retry and the different network timeouts that we have, including the network
 * partition detection timeout.
 *
 * In the initialisation code path though, pg_autoctl might be launched from
 * provisioning script on a set of nodes in parallel, and in that case we need
 * to secure a connection and implement a retry policy at the point in the code
 * where we open a connection, so that it's transparent to the caller.
 *
 * When we do retry connecting, we implement an Exponential Backoff with
 * Decorrelated Jitter algorithm as proven useful in the following article:
 *
 *  https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
 */
typedef struct ConnectionRetryPolicy
{
	int maxT;                   /* maximum time spent retrying (seconds) */
	int maxR;                   /* maximum number of retries, might be zero */
	int maxSleepTime;           /* in millisecond, used to cap sleepTime */
	int baseSleepTime;          /* in millisecond, base time to sleep for */
	int sleepTime;              /* in millisecond, time waited for last round */

	instr_time startTime;       /* time of the first attempt */
	instr_time connectTime;     /* time of successful connection */
	int attempts;               /* how many attempts have been made so far */
} ConnectionRetryPolicy;


/*
 * Allow higher level code to distinguish between failure to connect to the
 * target Postgres service and failure to run a query or obtain the expected
 * result. To that end we expose PQstatus() of the connection.
 *
 * We don't use the same enum values as in libpq because we want to have the
 * unknown value when we didn't try to connect yet.
 */
typedef enum
{
	PG_CONNECTION_UNKNOWN = 0,
	PG_CONNECTION_OK,
	PG_CONNECTION_BAD
} PGConnStatus;

/* notification processing */
typedef bool (*ProcessNotificationFunction)(int notificationGroupId,
											int notificationNodeId,
											char *channel, char *payload);

typedef struct PGSQL
{
	ConnectionType connectionType;
	char connectionString[MAXCONNINFO];
	PGconn *connection;
	ConnectionRetryPolicy retryPolicy;
	PGConnStatus status;

	ProcessNotificationFunction notificationProcessFunction;
	int notificationGroupId;
	int notificationNodeId;
	bool notificationReceived;
} PGSQL;


/* PostgreSQL ("Grand Unified Configuration") setting */
typedef struct GUC
{
	char *name;
	char *value;
} GUC;

/* network address of a node in an HA group */
typedef struct NodeAddress
{
	int nodeId;
	char name[_POSIX_HOST_NAME_MAX];
	char host[_POSIX_HOST_NAME_MAX];
	int port;
	char lsn[PG_LSN_MAXLENGTH];
	bool isPrimary;
} NodeAddress;

typedef struct NodeAddressArray
{
	int count;
	NodeAddress nodes[NODE_ARRAY_MAX_COUNT];
} NodeAddressArray;


/*
 * The replicationSource structure is used to pass the bits of a connection
 * string to the primary node around in several function calls. All the
 * information stored in there must fit in a connection string, so MAXCONNINFO
 * is a good proxy for their maximum size.
 */
typedef struct ReplicationSource
{
	NodeAddress primaryNode;
	char userName[NAMEDATALEN];
	char slotName[MAXCONNINFO];
	char password[MAXCONNINFO];
	char maximumBackupRate[MAXIMUM_BACKUP_RATE_LEN];
	char backupDir[MAXCONNINFO];
	char applicationName[MAXCONNINFO];
	char targetLSN[PG_LSN_MAXLENGTH];
	char targetAction[NAMEDATALEN];
	char targetTimeline[NAMEDATALEN];
	SSLOptions sslOptions;
} ReplicationSource;


/*
 * Arrange a generic way to parse PostgreSQL result from a query. Most of the
 * queries we need here return a single row of a single column, so that's what
 * the default context and parsing allows for.
 */

/* callback for parsing query results */
typedef void (ParsePostgresResultCB)(void *context, PGresult *result);

typedef enum
{
	PGSQL_RESULT_BOOL = 1,
	PGSQL_RESULT_INT,
	PGSQL_RESULT_BIGINT,
	PGSQL_RESULT_STRING
} QueryResultType;

/*
 * As a way to communicate the SQL STATE when an error occurs, every
 * pgsql_execute_with_params context structure must have the same first field,
 * an array of 5 characters (plus '\0' at the end).
 */
#define SQLSTATE_LENGTH 6

#define STR_ERRCODE_CLASS_CONNECTION_EXCEPTION "08"

typedef struct AbstractResultContext
{
	char sqlstate[SQLSTATE_LENGTH];
} AbstractResultContext;

/* data structure for keeping a single-value query result */
typedef struct SingleValueResultContext
{
	char sqlstate[SQLSTATE_LENGTH];
	QueryResultType resultType;
	bool parsedOk;
	bool boolVal;
	int intVal;
	uint64_t bigint;
	char *strVal;
} SingleValueResultContext;


#define CHECK__SETTINGS_SQL \
	"select bool_and(ok) " \
	"from (" \
	"select current_setting('max_wal_senders')::int >= 12" \
	" union all " \
	"select current_setting('max_replication_slots')::int >= 12" \
	" union all " \
	"select current_setting('wal_level') in ('replica', 'logical')" \
	" union all " \
	"select current_setting('wal_log_hints') = 'on'"

#define CHECK_POSTGRESQL_NODE_SETTINGS_SQL \
	CHECK__SETTINGS_SQL \
	") as t(ok) "

#define CHECK_CITUS_NODE_SETTINGS_SQL \
	CHECK__SETTINGS_SQL \
	" union all " \
	"select lib = 'citus' " \
	"from unnest(string_to_array(" \
	"current_setting('shared_preload_libraries'), ',') " \
	" || array['not citus']) " \
	"with ordinality ast(lib, n) where n = 1" \
	") as t(ok) "

bool pgsql_init(PGSQL *pgsql, char *url, ConnectionType connectionType);

void pgsql_set_retry_policy(ConnectionRetryPolicy *retryPolicy,
							int maxT,
							int maxR,
							int maxSleepTime,
							int baseSleepTime);
void pgsql_set_main_loop_retry_policy(ConnectionRetryPolicy *retryPolicy);
void pgsql_set_init_retry_policy(ConnectionRetryPolicy *retryPolicy);
void pgsql_set_interactive_retry_policy(ConnectionRetryPolicy *retryPolicy);
void pgsql_set_monitor_interactive_retry_policy(ConnectionRetryPolicy *retryPolicy);
int pgsql_compute_connection_retry_sleep_time(ConnectionRetryPolicy *retryPolicy);
bool pgsql_retry_policy_expired(ConnectionRetryPolicy *retryPolicy);

void pgsql_finish(PGSQL *pgsql);
void parseSingleValueResult(void *ctx, PGresult *result);
void fetchedRows(void *ctx, PGresult *result);
bool pgsql_execute(PGSQL *pgsql, const char *sql);
bool pgsql_execute_with_params(PGSQL *pgsql, const char *sql, int paramCount,
							   const Oid *paramTypes, const char **paramValues,
							   void *parseContext, ParsePostgresResultCB *parseFun);
bool pgsql_check_postgresql_settings(PGSQL *pgsql, bool isCitusInstanceKind,
									 bool *settings_are_ok);
bool pgsql_check_monitor_settings(PGSQL *pgsql, bool *settings_are_ok);
bool pgsql_is_in_recovery(PGSQL *pgsql, bool *is_in_recovery);
bool pgsql_reload_conf(PGSQL *pgsql);
bool pgsql_replication_slot_exists(PGSQL *pgsql, const char *slotName,
								   bool *slotExists);
bool pgsql_create_replication_slot(PGSQL *pgsql, const char *slotName);
bool pgsql_drop_replication_slot(PGSQL *pgsql, const char *slotName);
bool postgres_sprintf_replicationSlotName(int nodeId, char *slotName, int size);
bool pgsql_set_synchronous_standby_names(PGSQL *pgsql,
										 char *synchronous_standby_names);
bool pgsql_replication_slot_create_and_drop(PGSQL *pgsql,
											NodeAddressArray *nodeArray);
bool pgsql_replication_slot_maintain(PGSQL *pgsql, NodeAddressArray *nodeArray);
bool postgres_sprintf_replicationSlotName(int nodeId, char *slotName, int size);
bool pgsql_disable_synchronous_replication(PGSQL *pgsql);
bool pgsql_set_default_transaction_mode_read_only(PGSQL *pgsql);
bool pgsql_set_default_transaction_mode_read_write(PGSQL *pgsql);
bool pgsql_checkpoint(PGSQL *pgsql);
bool pgsql_get_hba_file_path(PGSQL *pgsql, char *hbaFilePath, int maxPathLength);
bool pgsql_create_database(PGSQL *pgsql, const char *dbname, const char *owner);
bool pgsql_create_extension(PGSQL *pgsql, const char *name);
bool pgsql_create_user(PGSQL *pgsql, const char *userName, const char *password,
					   bool login, bool superuser, bool replication,
					   int connlimit);
bool pgsql_has_replica(PGSQL *pgsql, char *userName, bool *hasReplica);
bool hostname_from_uri(const char *pguri,
					   char *hostname, int maxHostLength, int *port);
bool validate_connection_string(const char *connectionString);
bool pgsql_reset_primary_conninfo(PGSQL *pgsql);

bool pgsql_get_postgres_metadata(PGSQL *pgsql,
								 bool *pg_is_in_recovery,
								 char *pgsrSyncState, char *currentLSN,
								 PostgresControlData *control);

bool pgsql_one_slot_has_reached_target_lsn(PGSQL *pgsql,
										   char *targetLSN,
										   char *currentLSN,
										   bool *hasReachedLSN);
bool pgsql_has_reached_target_lsn(PGSQL *pgsql, char *targetLSN,
								  char *currentLSN, bool *hasReachedLSN);
bool pgsql_identify_system(PGSQL *pgsql);
bool pgsql_listen(PGSQL *pgsql, char *channels[]);

bool pgsql_alter_extension_update_to(PGSQL *pgsql,
									 const char *extname, const char *version);


#endif /* PGSQL_H */

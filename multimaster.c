/*
 * multimaster.c
 *
 * Multimaster based on logical replication
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "postgres.h"
#include "funcapi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pg_socket.h"

#include "libpq-fe.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "common/username.h"

#include "postmaster/postmaster.h"
#include "postmaster/bgworker.h"
#include "storage/lwlock.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "access/xlogdefs.h"
#include "access/xact.h"
#include "access/xtm.h"
#include "access/transam.h"
#include "access/subtrans.h"
#include "access/commit_ts.h"
#include "access/xlog.h"
#include "storage/proc.h"
#include "executor/executor.h"
#include "access/twophase.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/timeout.h"
#include "utils/tqual.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "commands/sequence.h"
#include "postmaster/autovacuum.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "replication/slot.h"
#include "replication/message.h"
#include "port/atomics.h"
#include "tcop/utility.h"
#include "nodes/makefuncs.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_constraint_fn.h"
#include "catalog/pg_proc.h"
#include "pglogical_output/hooks.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_func.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "tcop/pquery.h"
#include "lib/ilist.h"

#include "multimaster.h"
#include "ddd.h"
#include "state.h"

typedef struct {
	TransactionId xid;	  /* local transaction ID	*/
	GlobalTransactionId gtid; /* global transaction ID assigned by coordinator of transaction */
	bool  isTwoPhase;	  /* user level 2PC */
	bool  isReplicated;	  /* transaction on replica */
	bool  isDistributed;  /* transaction performed INSERT/UPDATE/DELETE and has to be replicated to other nodes */
	bool  isPrepared;	  /* transaction is prepared at first stage of 2PC */
	bool  isSuspended;	  /* prepared transaction is suspended because coordinator node is switch to offline */
	bool  isTransactionBlock; /* is transaction block */
	bool  containsDML;	  /* transaction contains DML statements */
	bool  isActive;		  /* transaction is active (nActiveTransaction counter is incremented) */
	XidStatus status;	  /* transaction status */
	csn_t snapshot;		  /* transaction snapshot */
	csn_t csn;			  /* CSN */
	pgid_t gid;			  /* global transaction identifier (used by 2pc) */
} MtmCurrentTrans;

typedef enum
{
	MTM_STATE_LOCK_ID
} MtmLockIds;

#define MTM_SHMEM_SIZE (128*1024*1024)
#define MTM_HASH_SIZE  100003
#define MTM_MAP_SIZE   MTM_HASH_SIZE
#define MIN_WAIT_TIMEOUT 1000
#define MAX_WAIT_TIMEOUT 100000
#define MAX_WAIT_LOOPS	 10000 // 1000000
#define STATUS_POLL_DELAY USECS_PER_SEC

void _PG_init(void);
void _PG_fini(void);

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(mtm_start_replication);
PG_FUNCTION_INFO_V1(mtm_stop_replication);
PG_FUNCTION_INFO_V1(mtm_stop_node);
PG_FUNCTION_INFO_V1(mtm_add_node);
PG_FUNCTION_INFO_V1(mtm_poll_node);
PG_FUNCTION_INFO_V1(mtm_recover_node);
PG_FUNCTION_INFO_V1(mtm_resume_node);
PG_FUNCTION_INFO_V1(mtm_get_snapshot);
PG_FUNCTION_INFO_V1(mtm_get_csn);
PG_FUNCTION_INFO_V1(mtm_get_trans_by_gid);
PG_FUNCTION_INFO_V1(mtm_get_trans_by_xid);
PG_FUNCTION_INFO_V1(mtm_get_last_csn);
PG_FUNCTION_INFO_V1(mtm_get_nodes_state);
PG_FUNCTION_INFO_V1(mtm_get_cluster_state);
PG_FUNCTION_INFO_V1(mtm_collect_cluster_info);
PG_FUNCTION_INFO_V1(mtm_make_table_local);
PG_FUNCTION_INFO_V1(mtm_dump_lock_graph);
PG_FUNCTION_INFO_V1(mtm_inject_2pc_error);
PG_FUNCTION_INFO_V1(mtm_check_deadlock);
PG_FUNCTION_INFO_V1(mtm_referee_poll);
PG_FUNCTION_INFO_V1(mtm_broadcast_table);
PG_FUNCTION_INFO_V1(mtm_copy_table);

static Snapshot MtmGetSnapshot(Snapshot snapshot);
static void MtmInitialize(void);
static void MtmXactCallback(XactEvent event, void *arg);
static void MtmBeginTransaction(MtmCurrentTrans* x);
static void MtmPrePrepareTransaction(MtmCurrentTrans* x);
static void MtmPostPrepareTransaction(MtmCurrentTrans* x);
static void MtmAbortPreparedTransaction(MtmCurrentTrans* x);
static void MtmPreCommitPreparedTransaction(MtmCurrentTrans* x);
static void MtmEndTransaction(MtmCurrentTrans* x, bool commit);
static bool MtmTwoPhaseCommit(MtmCurrentTrans* x);
static TransactionId MtmGetOldestXmin(Relation rel, bool ignoreVacuum);
// static bool MtmXidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);
static void MtmAdjustOldestXid(void);
static bool MtmDetectGlobalDeadLock(PGPROC* proc);
static void MtmAddSubtransactions(MtmTransState* ts, TransactionId* subxids, int nSubxids);
static char const* MtmGetName(void);
static size_t MtmGetTransactionStateSize(void);
static void	  MtmSerializeTransactionState(void* ctx);
static void	  MtmDeserializeTransactionState(void* ctx);
static void	  MtmInitializeSequence(int64* start, int64* step);
static void*  MtmCreateSavepointContext(void);
static void	  MtmRestoreSavepointContext(void* ctx);
static void	  MtmReleaseSavepointContext(void* ctx);
static void   MtmSetRemoteFunction(char const* list, void* extra);

// static void MtmCheckClusterLock(void);
static void MtmCheckSlots(void);
static void MtmAddSubtransactions(MtmTransState* ts, TransactionId *subxids, int nSubxids);

static void MtmShmemStartup(void);

static BgwPool* MtmPoolConstructor(void);
static bool MtmRunUtilityStmt(PGconn* conn, char const* sql, char **errmsg);
static void MtmBroadcastUtilityStmt(char const* sql, bool ignoreError, int forceOnNode);
static void MtmProcessDDLCommand(char const* queryString, bool transactional);

// static void MtmLockCluster(void);
// static void MtmUnlockCluster(void);

MtmState* Mtm;

VacuumStmt* MtmVacuumStmt;
IndexStmt*	MtmIndexStmt;
DropStmt*	MtmDropStmt;
void*		MtmTablespaceStmt; /* CREATE/DELETE tablespace */
MemoryContext MtmApplyContext;
MtmConnectionInfo* MtmConnections;

HTAB* MtmXid2State;
HTAB* MtmGid2State;
static HTAB* MtmRemoteFunctions;
static HTAB* MtmLocalTables;

static bool MtmIsRecoverySession;

static MtmCurrentTrans MtmTx;
static dlist_head MtmLsnMapping = DLIST_STATIC_INIT(MtmLsnMapping);

static TransactionManager MtmTM =
{
	PgTransactionIdGetStatus,
	PgTransactionIdSetTreeStatus,
	MtmGetSnapshot,
	PgGetNewTransactionId,
	MtmGetOldestXmin,
	PgTransactionIdIsInProgress,
	PgGetGlobalTransactionId,
	PgXidInMVCCSnapshot,
	MtmDetectGlobalDeadLock,
	MtmGetName,
	MtmGetTransactionStateSize,
	MtmSerializeTransactionState,
	MtmDeserializeTransactionState,
	MtmInitializeSequence,
	MtmCreateSavepointContext,
	MtmRestoreSavepointContext,
	MtmReleaseSavepointContext
};

char const* const MtmNodeStatusMnem[] =
{
	"Disabled",
	"Recovery",
	"Recovered",
	"Online"
};

char const* const MtmTxnStatusMnem[] =
{
	"InProgress",
	"Committed",
	"Aborted",
	"Unknown"
};

bool  MtmDoReplication;
char* MtmDatabaseName;
char* MtmDatabaseUser;
Oid	  MtmDatabaseId;
bool  MtmBackgroundWorker;

int	  MtmNodes;
int	  MtmNodeId;
int	  MtmReplicationNodeId;
int	  MtmArbiterPort;
int	  MtmNodeDisableDelay;
int	  MtmTransSpillThreshold;
int	  MtmMaxNodes;
int	  MtmHeartbeatSendTimeout;
int	  MtmHeartbeatRecvTimeout;
int	  MtmMin2PCTimeout;
int	  MtmMax2PCRatio;
bool  MtmUseDtm;
bool  MtmUseRDMA;
bool  MtmPreserveCommitOrder;
bool  MtmVolksWagenMode; /* Pretend to be normal postgres. This means skip some NOTICE's and use local sequences */
bool  MtmMajorNode;
char* MtmRefereeConnStr;
bool  MtmEnforceLocalTx;

static char* MtmConnStrs;
static char* MtmRemoteFunctionsList;
static char* MtmClusterName;
static int	 MtmQueueSize;
static int	 MtmWorkers;
static int	 MtmVacuumDelay;
static int	 MtmMinRecoveryLag;
static int	 MtmMaxRecoveryLag;
static int	 MtmGcPeriod;
static bool	 MtmIgnoreTablesWithoutPk;
static int	 MtmLockCount;
static bool	 MtmBreakConnection;
static bool  MtmBypass;
static bool	 MtmClusterLocked;
static bool	 MtmInsideTransaction;
static bool  MtmReferee;
static bool  MtmMonotonicSequences;
static void const* MtmDDLStatement;

static ExecutorStart_hook_type PreviousExecutorStartHook;
static ExecutorFinish_hook_type PreviousExecutorFinishHook;
static ProcessUtility_hook_type PreviousProcessUtilityHook;
static shmem_startup_hook_type PreviousShmemStartupHook;
static seq_nextval_hook_t PreviousSeqNextvalHook;

static void MtmExecutorStart(QueryDesc *queryDesc, int eflags);
static void MtmExecutorFinish(QueryDesc *queryDesc);
static void MtmProcessUtility(Node *parsetree, const char *queryString,
							 ProcessUtilityContext context, ParamListInfo params,
							 DestReceiver *dest, char *completionTag);
static void MtmSeqNextvalHook(Oid seqid, int64 next);

static bool MtmAtExitHookRegistered = false;

/*
 * Release multimaster main lock if been hold.
 * This function is called when backend is terminated because of critical error or when error is catched
 * by FINALLY block
 */
void MtmReleaseLocks(void)
{
	MtmResetTransaction();
	if (MtmInsideTransaction)
	{
		MtmLock(LW_EXCLUSIVE);
		Assert(Mtm->nRunningTransactions > 0);
		Mtm->nRunningTransactions -= 1;
		MtmInsideTransaction = false;
		MtmUnlock();
	}
	// if (MtmClusterLocked) {
	// 	MtmUnlockCluster();
	// }
}

/*
 * -------------------------------------------
 * Synchronize access to MTM structures.
 * Using LWLock seems to be more efficient (at our benchmarks)
 * Multimaster uses trash of 2N+1 lwlocks, where N is number of nodes.
 * locks[0] is used to synchronize access to multimaster state,
 * locks[1..N] are used to provide exclusive access to replication session for each node
 * locks[N+1..2*N] are used to synchronize access to distributed lock graph at each node
 * -------------------------------------------
 */

// #define DEBUG_MTM_LOCK 1

#if DEBUG_MTM_LOCK
static timestamp_t MtmLockLastReportTime;
static timestamp_t MtmLockElapsedWaitTime;
static timestamp_t MtmLockMaxWaitTime;
static size_t      MtmLockHitCount;
#endif

void MtmLock(LWLockMode mode)
{
	if (!MtmAtExitHookRegistered) {
		atexit(MtmReleaseLocks);
		MtmAtExitHookRegistered = true;
	}
	if (MtmLockCount != 0 && Mtm->lastLockHolder == MyProcPid) {
		MtmLockCount += 1;
	}
	else
	{
#if DEBUG_MTM_LOCK
		timestamp_t start, stop;
		start = MtmGetSystemTime();
#endif
		if (MyProc == NULL) { /* Can not wait if have no PGPROC. It can happen at process exit. TODO: without lock we can get race condition and corrupt Mtm state */
			return;
		}
		LWLockAcquire((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID], mode);
#if DEBUG_MTM_LOCK
		stop = MtmGetSystemTime();
		MtmLockElapsedWaitTime += stop - start;
		if (stop - start > MtmLockMaxWaitTime) {
			MtmLockMaxWaitTime = stop - start;
		}
		MtmLockHitCount += 1;
		if (stop - MtmLockLastReportTime > USECS_PER_SEC) {
			MTM_LOG1("%d: average lock wait time %lld usec, maximal lock wait time: %lld usec",
					 MyProcPid, MtmLockElapsedWaitTime/MtmLockHitCount, MtmLockMaxWaitTime);
			MtmLockLastReportTime = stop;
			MtmLockMaxWaitTime = 0;
			MtmLockElapsedWaitTime = 0;
			MtmLockHitCount = 0;
		}
#endif
		if (mode == LW_EXCLUSIVE) {
			Assert(MtmLockCount == 0);
			Assert(MyProcPid != 0);
			Mtm->lastLockHolder = MyProcPid;
			Assert(MyProcPid);
			MtmLockCount = 1;
		} else {
			MtmLockCount = 0;
		}
	}
}

void MtmUnlock(void)
{
	if (MtmLockCount != 0 && --MtmLockCount != 0) {
		Assert(Mtm->lastLockHolder == MyProcPid);
		return;
	}

	Mtm->lastLockHolder = 0;

	/* If we have no PGPROC, then lock was not obtained. */
	if (MyProc != NULL)
		LWLockRelease((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID]);
}

void MtmDeepUnlock(void)
{
	if (MtmLockCount == 0)
		return;

	Assert(Mtm->lastLockHolder == MyProcPid);

	MtmLockCount = 0;
	Mtm->lastLockHolder = 0;

	/* If we have no PGPROC, then lock was not obtained. */
	if (MyProc != NULL)
		LWLockRelease((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID]);
}

void MtmLockNode(int nodeId, LWLockMode mode)
{
	Assert(nodeId > 0 && nodeId <= MtmMaxNodes*2);
	LWLockAcquire((LWLockId)&Mtm->locks[nodeId], mode);
}

bool MtmTryLockNode(int nodeId, LWLockMode mode)
{
	return LWLockConditionalAcquire((LWLockId)&Mtm->locks[nodeId], mode);
}

void MtmUnlockNode(int nodeId)
{
	Assert(nodeId > 0 && nodeId <= MtmMaxNodes*2);
	LWLockRelease((LWLockId)&Mtm->locks[nodeId]);
}

/*
 * -------------------------------------------
 * System time manipulation functions
 * -------------------------------------------
 */


timestamp_t MtmGetSystemTime(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (timestamp_t)tv.tv_sec*USECS_PER_SEC + tv.tv_usec;
}

/*
 * Get adjusted system time: taking in account time shift
 */
timestamp_t MtmGetCurrentTime(void)
{
	return MtmGetSystemTime() + Mtm->timeShift;
}

void MtmSleep(timestamp_t interval)
{
	struct timespec ts;
	struct timespec rem;
	ts.tv_sec = interval/USECS_PER_SEC;
	ts.tv_nsec = interval%USECS_PER_SEC*1000;

	while (nanosleep(&ts, &rem) < 0) {
		Assert(errno == EINTR);
		CHECK_FOR_INTERRUPTS();
		ts = rem;
	}
}

/**
 * Return ascending unique timestamp which is used as CSN
 */
csn_t MtmAssignCSN()
{
	csn_t csn = MtmGetCurrentTime();
	if (csn <= Mtm->csn) {
		csn = ++Mtm->csn;
	} else {
		Mtm->csn = csn;
	}
	return csn;
}

/**
 * "Adjust" system clock if we receive message from future
 */
csn_t MtmSyncClock(csn_t global_csn)
{
	csn_t local_csn;
	while ((local_csn = MtmAssignCSN()) < global_csn) {
		Mtm->timeShift += global_csn - local_csn;
	}
	return local_csn;
}

/*
 * Distribute transaction manager functions
 */
static char const* MtmGetName(void)
{
	return MULTIMASTER_NAME;
}

static size_t
MtmGetTransactionStateSize(void)
{
	return sizeof(MtmTx);
}

static void
MtmSerializeTransactionState(void* ctx)
{
	memcpy(ctx, &MtmTx, sizeof(MtmTx));
}

static void
MtmDeserializeTransactionState(void* ctx)
{
	memcpy(&MtmTx, ctx, sizeof(MtmTx));
}


static void
MtmInitializeSequence(int64* start, int64* step)
{
	if (MtmVolksWagenMode)
	{
		*start = 1;
		*step  = 1;
	}
	else
	{
		*start = MtmNodeId;
		*step  = MtmMaxNodes;
	}
}

static void* MtmCreateSavepointContext(void)
{
	return (void*)(size_t)MtmTx.containsDML;
}

static void	 MtmRestoreSavepointContext(void* ctx)
{
	MtmTx.containsDML = ctx != NULL;
}

static void	 MtmReleaseSavepointContext(void* ctx)
{
}


/*
 * -------------------------------------------
 * Visibility&snapshots
 * -------------------------------------------
 */

/*
 * Get snapshot of transaction proceed by WAL sender pglogical plugin.
 * If it is local transaction or replication node is not in participant mask, then return INVALID_CSN.
 * Transaction should be skipped by WAL sender in the following cases:
 *	 1. Transaction was replicated from some other node and it is not a recovery process.
 *	 2. State of transaction is unknown
 *	 3. Replication node is not participated in transaction
 */
csn_t MtmDistributedTransactionSnapshot(TransactionId xid, int nodeId, nodemask_t* participantsMask)
{
	csn_t snapshot = INVALID_CSN;
	*participantsMask = 0;
	MtmLock(LW_SHARED);
	if (Mtm->status == MTM_ONLINE) {
		MtmTransState* ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
		if (ts != NULL) {
			*participantsMask = ts->participantsMask;
			/* If node is disables, then we are in a process of recovery of this node */
			if (!ts->isLocal && BIT_CHECK(ts->participantsMask|Mtm->disabledNodeMask, nodeId-1)) {
				snapshot = ts->snapshot;
				Assert(ts->gtid.node == MtmNodeId || MtmIsRecoverySession);
			} else {
				MTM_LOG1("Do not send transaction %s (%llu) to node %d participants mask %llx",
						 ts->gid, (long64)ts->xid, nodeId, ts->participantsMask);
			}
		}
	}
	MtmUnlock();
	return snapshot;
}

void MtmSetSnapshot(csn_t globalSnapshot)
{
	MtmLock(LW_EXCLUSIVE);
	MtmSyncClock(globalSnapshot);
	MtmTx.snapshot = globalSnapshot;
	MtmUnlock();
}


Snapshot MtmGetSnapshot(Snapshot snapshot)
{
	snapshot = PgGetSnapshotData(snapshot);
	if (XactIsoLevel == XACT_READ_COMMITTED && MtmTx.snapshot != INVALID_CSN) {
		MtmTx.snapshot = MtmGetCurrentTime();
		if (TransactionIdIsValid(GetCurrentTransactionIdIfAny())) {
			LogLogicalMessage("S", (char*)&MtmTx.snapshot, sizeof(MtmTx.snapshot), true);
		}
	}
	// RecentGlobalDataXmin = RecentGlobalXmin = Mtm->oldestXid;
	return snapshot;
}


TransactionId MtmGetOldestXmin(Relation rel, bool ignoreVacuum)
{
	TransactionId xmin = PgGetOldestXmin(rel, ignoreVacuum); /* consider all backends */
	// if (TransactionIdIsValid(xmin)) {
	// 	MtmLock(LW_EXCLUSIVE);
	// 	xmin = MtmAdjustOldestXid(xmin);
	// 	MtmUnlock();
	// }
	return xmin;
}

// bool MtmXidInMVCCSnapshot(TransactionId xid, Snapshot snapshot)
// {
// #if TRACE_SLEEP_TIME
// 	static timestamp_t firstReportTime;
// 	static timestamp_t prevReportTime;
// 	static timestamp_t totalSleepTime;
// 	static timestamp_t maxSleepTime;
// #endif
// 	timestamp_t delay = MIN_WAIT_TIMEOUT;
// 	int i;
// #if DEBUG_LEVEL > 1
// 	timestamp_t start = MtmGetSystemTime();
// #endif

// 	Assert(xid != InvalidTransactionId);

// 	if (!MtmUseDtm || TransactionIdPrecedes(xid, Mtm->oldestXid)) {
// 		return PgXidInMVCCSnapshot(xid, snapshot);
// 	}
// 	MtmLock(LW_SHARED);

// #if TRACE_SLEEP_TIME
// 	if (firstReportTime == 0) {
// 		firstReportTime = MtmGetCurrentTime();
// 	}
// #endif

// 	for (i = 0; i < MAX_WAIT_LOOPS; i++)
// 	{
// 		csn_t csn;
// 		RepOriginId reporigin_id;
// 		MtmTransState* ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
// 		if (ts != NULL /*&& ts->status != TRANSACTION_STATUS_IN_PROGRESS*/)
// 		{

// 			if (ts->status == TRANSACTION_STATUS_UNKNOWN || ts->status == TRANSACTION_STATUS_IN_PROGRESS)
// 				csn = ts->csn;
// 			else
// 				TransactionIdGetCommitTsData(xid, &csn, &reporigin_id);

// 			if (ts->csn != csn && ts->status != TRANSACTION_STATUS_ABORTED)
// 			{
// 				MTM_LOG1("WOW! %d: tuple with xid=%lld(csn=%ld / %ld) woes in snapshot %ld (s=%d)", MyProcPid, (long64)xid, csn, ts->csn, MtmTx.snapshot, ts->status);
// 			}

// 			if (csn > MtmTx.snapshot) {
// 				MTM_LOG4("%d: tuple with xid=%lld(csn=%lld) is invisible in snapshot %lld",
// 						 MyProcPid, (long64)xid, ts->csn, MtmTx.snapshot);
// #if DEBUG_LEVEL > 1
// 				if (MtmGetSystemTime() - start > USECS_PER_SEC) {
// 					MTM_ELOG(WARNING, "Backend %d waits for transaction %s (%llu) status %lld usecs", MyProcPid, ts->gid, (long64)xid, MtmGetSystemTime() - start);
// 				}
// #endif
// 				MtmUnlock();
// 				return true;
// 			}
// 			if (ts->status == TRANSACTION_STATUS_UNKNOWN)
// 			{
// 				MTM_LOG3("%d: wait for in-doubt transaction %u in snapshot %llu", MyProcPid, xid, MtmTx.snapshot);
// 				MtmUnlock();
// #if TRACE_SLEEP_TIME
// 				{
// 				timestamp_t delta, now = MtmGetCurrentTime();
// #endif
// 				MtmSleep(delay);
// #if TRACE_SLEEP_TIME
// 				delta = MtmGetCurrentTime() - now;
// 				totalSleepTime += delta;
// 				if (delta > maxSleepTime) {
// 					maxSleepTime = delta;
// 				}
// 				if (now > prevReportTime + USECS_PER_SEC*10) {
// 					prevReportTime = now;
// 					if (firstReportTime == 0) {
// 						firstReportTime = now;
// 					} else {
// 						MTM_LOG3("Snapshot sleep %llu of %llu usec (%f%%), maximum=%llu", totalSleepTime, now - firstReportTime, totalSleepTime*100.0/(now - firstReportTime), maxSleepTime);
// 					}
// 				}
// 				}
// #endif
// 				if (delay*2 <= MAX_WAIT_TIMEOUT) {
// 					delay *= 2;
// 				}
// 				MtmLock(LW_SHARED);
// 			}
// 			else
// 			{
// 				bool invisible = ts->status != TRANSACTION_STATUS_COMMITTED;
// 				MTM_LOG4("%d: tuple with xid=%lld(csn= %lld) is %s in snapshot %lld",
// 						 MyProcPid, (long64)xid, ts->csn, invisible ? "rollbacked" : "committed", MtmTx.snapshot);
// 				MtmUnlock();
// #if DEBUG_LEVEL > 1
// 				if (MtmGetSystemTime() - start > USECS_PER_SEC) {
// 					MTM_ELOG(WARNING, "Backend %d waits for %s transaction %s (%llu) %lld usecs", MyProcPid, invisible ? "rollbacked" : "committed",
// 						 ts->gid, (long64)xid, MtmGetSystemTime() - start);
// 				}
// #endif
// 				return invisible;
// 			}
// 		}
// 		else
// 		{
// 			MTM_LOG4("%d: visibility check is skipped for transaction %llu in snapshot %llu", MyProcPid, (long64)xid, MtmTx.snapshot);
// 			MtmUnlock();
// 			return PgXidInMVCCSnapshot(xid, snapshot);
// 		}
// 	}
// 	MtmUnlock();
// #if DEBUG_LEVEL > 1
// 	MTM_ELOG(ERROR, "Failed to get status of XID %llu in %lld usec", (long64)xid, MtmGetSystemTime() - start);
// #else
// 	MTM_ELOG(ERROR, "Failed to get status of XID %llu", (long64)xid);
// #endif
// 	return true;
// }



/*
 * There can be different oldest XIDs at different cluster node.
 * We collect oldest CSNs from all nodes and choose minimum from them.
 * If no such XID can be located, then return previously observed oldest XID
 */
static void
MtmAdjustOldestXid(void)
{
	MtmL2List *start = Mtm->activeTransList.next;
	MtmL2List *cur;

	for (cur = start; cur->next != start; cur = cur->next)
	{
		MtmTransState *ts = MtmGetActiveTransaction(cur);

		if ( (ts->status == TRANSACTION_STATUS_ABORTED
				|| ts->status == TRANSACTION_STATUS_COMMITTED)
			 && !ts->isActive
			 && !ts->isPinned
			 && ts->csn < (MtmGetCurrentTime() - MtmVacuumDelay*USECS_PER_SEC) )
		{
			ts->activeList.prev->next = ts->activeList.next;
			ts->activeList.next->prev = ts->activeList.prev;
			if (cur == start)
				start = cur->next;
			hash_search(MtmXid2State, &ts->xid, HASH_REMOVE, NULL);
			hash_search(MtmGid2State, &ts->gid, HASH_REMOVE, NULL);
		}
		else
		{
			if (TransactionIdPrecedes(ts->xid, Mtm->oldestXid))
				Mtm->oldestXid = ts->xid;
		}
	}

	Mtm->gcCount = 0;

	if (!MyReplicationSlot) {
		MtmCheckSlots();
	}
}




/*
 * -------------------------------------------
 * Transaction list manipulation.
 * All distributed transactions are linked in L1-list ordered by transaction start time.
 * This list is inspected by MtmAdjustOldestXid and transactions which are not used in any snapshot at any node
 * are removed from the list and from the hash.
 * -------------------------------------------
 */

static void MtmTransactionListAppend(MtmTransState* ts)
{
	if (!ts->isEnqueued) {
		ts->isEnqueued = true;
		ts->next = NULL;
		ts->nSubxids = 0;
		*Mtm->transListTail = ts;
		Mtm->transListTail = &ts->next;
	}
}

static void MtmTransactionListInsertAfter(MtmTransState* after, MtmTransState* ts)
{
	ts->next = after->next;
	after->next = ts;
	ts->isEnqueued = true;
	if (Mtm->transListTail == &after->next) {
		Mtm->transListTail = &ts->next;
	}
}

static void MtmAddSubtransactions(MtmTransState* ts, TransactionId* subxids, int nSubxids)
{
	int i;
	ts->nSubxids = nSubxids;
	for (i = 0; i < nSubxids; i++) {
		bool found;
		MtmTransState* sts;
		Assert(TransactionIdIsValid(subxids[i]));
		sts = (MtmTransState*)hash_search(MtmXid2State, &subxids[i], HASH_ENTER, &found);
		Assert(!found);
		sts->isActive = false;
		sts->isPinned = false;
		sts->status = ts->status;
		sts->csn = ts->csn;
		sts->votingCompleted = true;
		MtmTransactionListInsertAfter(ts, sts);
	}
}

void MtmAdjustSubtransactions(MtmTransState* ts)
{
	int i;
	int nSubxids = ts->nSubxids;
	MtmTransState* sts = ts;

	for (i = 0; i < nSubxids; i++) {
		sts = sts->next;
		sts->status = ts->status;
		sts->csn = ts->csn;
	}
}

/*
 * -------------------------------------------
 * Transaction control
 * -------------------------------------------
 */


static void
MtmXactCallback(XactEvent event, void *arg)
{
	switch (event)
	{
	  case XACT_EVENT_START:
		MtmBeginTransaction(&MtmTx);
		break;
	  case XACT_EVENT_PRE_PREPARE:
		MtmPrePrepareTransaction(&MtmTx);
		break;
	  case XACT_EVENT_POST_PREPARE:
		MtmPostPrepareTransaction(&MtmTx);
		break;
	  case XACT_EVENT_ABORT_PREPARED:
		MtmAbortPreparedTransaction(&MtmTx);
		break;
	  case XACT_EVENT_PRE_COMMIT_PREPARED:
		MtmPreCommitPreparedTransaction(&MtmTx);
		break;
	  case XACT_EVENT_COMMIT:
		MtmEndTransaction(&MtmTx, true);
		break;
	  case XACT_EVENT_ABORT:
		MtmEndTransaction(&MtmTx, false);
		break;
	  case XACT_EVENT_COMMIT_COMMAND:
		if (!MtmTx.isTransactionBlock && !IsSubTransaction()) {
			MtmTwoPhaseCommit(&MtmTx);
		}
		break;
	  default:
		break;
	}
}

/*
 * Check if this is "normal" user transaction which should be distributed to other nodes
 */
static bool
MtmIsUserTransaction()
{
	return !IsAutoVacuumLauncherProcess() &&
		IsNormalProcessingMode() &&
		MtmDoReplication &&
		!am_walsender &&
		!MtmBackgroundWorker &&
		!IsAutoVacuumWorkerProcess();
}

void
MtmResetTransaction()
{
	MtmCurrentTrans* x = &MtmTx;
	x->snapshot = INVALID_CSN;
	x->xid = InvalidTransactionId;
	x->gtid.xid = InvalidTransactionId;
	x->isDistributed = false;
	x->isPrepared = false;
	x->isSuspended = false;
	x->isActive = false;
	x->isTwoPhase = false;
	x->csn = INVALID_CSN;
	x->status = TRANSACTION_STATUS_UNKNOWN;
	x->gid[0] = '\0';
	MtmDDLStatement = NULL;
}

#if 0
static const char* const isoLevelStr[] =
{
	"read uncommitted",
	"read committed",
	"repeatable read",
	"serializable"
};
#endif

bool MtmTransIsActive(void)
{
	return MtmTx.isActive;
}


static void
MtmBeginTransaction(MtmCurrentTrans* x)
{
	if (x->snapshot == INVALID_CSN) {

		Assert(!x->isActive);
		MtmLock(LW_EXCLUSIVE);
		if (Mtm->gcCount >= MtmGcPeriod) {
			MtmAdjustOldestXid();
		}

		x->xid = GetCurrentTransactionIdIfAny();
		x->isReplicated = MtmIsLogicalReceiver;
		x->isDistributed = MtmIsUserTransaction();
		x->isPrepared = false;
		x->isSuspended = false;
		x->isTwoPhase = false;
		x->isTransactionBlock = IsTransactionBlock();
		/* Application name can be changed using PGAPPNAME environment variable */
		if (x->isDistributed && Mtm->status != MTM_ONLINE && strcmp(application_name, MULTIMASTER_ADMIN) != 0
			&& strcmp(application_name, MULTIMASTER_BROADCAST_SERVICE) != 0
			&& !MtmBypass) {
			/* Reject all user's transactions at offline cluster.
			 * Allow execution of transaction by bg-workers to make it possible to perform recovery.
			 */
			MtmUnlock();
			MTM_ELOG(MtmBreakConnection ? FATAL : ERROR, "Multimaster node is not online: current status %s", MtmNodeStatusMnem[Mtm->status]);
		}
		x->containsDML = false;
		x->gtid.xid = InvalidTransactionId;
		x->gid[0] = '\0';
		x->status = TRANSACTION_STATUS_IN_PROGRESS;

		/*
		 * Check if there is global multimaster lock preventing new transaction from commit to make a chance to wal-senders to caught-up.
		 * Allow applying of replicated transactions to avoid deadlock (to caught-up we need active transaction counter to become zero).
		 * Also allow user to complete explicit 2PC transactions.
		 */
		if (x->isDistributed
			&& !MtmClusterLocked /* do not lock myself */
			&& strcmp(application_name, MULTIMASTER_ADMIN) != 0
			&& !MtmBypass)
		{
			// MtmCheckClusterLock();
		}
		MtmInsideTransaction = true;
		MtmDDLStatement = NULL;
		Mtm->nRunningTransactions += 1;

		x->snapshot = MtmAssignCSN();
		MTM_LOG2("Start transaction %lld with snapshot %lld", (long64)x->xid, x->snapshot);

		MtmUnlock();

		MTM_LOG3("%d: MtmLocalTransaction: %s transaction %u uses local snapshot %llu",
				 MyProcPid, x->isDistributed ? "distributed" : "local", x->xid, x->snapshot);
	} else {
		Assert(MtmInsideTransaction);
	}
}


static MtmTransState*
MtmCreateTransState(MtmCurrentTrans* x)
{
	bool found;
	MtmTransState* ts = (MtmTransState*)hash_search(MtmXid2State, &x->xid, HASH_ENTER, &found);
	ts->status = TRANSACTION_STATUS_IN_PROGRESS;
	ts->snapshot = x->snapshot;
	ts->isLocal = true;
	ts->isPrepared = false;
	ts->isTwoPhase = x->isTwoPhase;
	ts->isPinned = false;
	ts->votingCompleted = false;
	ts->abortedByNode = 0;
	if (!found) {
		ts->isEnqueued = false;
		ts->isActive = false;
	}
	if (TransactionIdIsValid(x->gtid.xid)) {
		Assert(x->gtid.node != MtmNodeId);
		ts->gtid = x->gtid;
	} else {
		/* I am coordinator of transaction */
		ts->gtid.xid = x->xid;
		ts->gtid.node = MtmNodeId;
	}
	strcpy(ts->gid, x->gid);
	x->isActive = true;
	return ts;
}

static void MtmActivateTransaction(MtmTransState* ts)
{
	if (!ts->isActive) {
		ts->activeList.next = Mtm->activeTransList.next;
		ts->activeList.prev = &Mtm->activeTransList;
		Mtm->activeTransList.next = Mtm->activeTransList.next->prev = &ts->activeList;
		ts->isActive = true;
		Mtm->nActiveTransactions += 1;
	}
}

static void MtmDeactivateTransaction(MtmTransState* ts)
{
	if (ts->isActive) {
		/*
		 * We'll clean that later during adjustOldestXid
		 */
		ts->isActive = false;
		Assert(Mtm->nActiveTransactions != 0);
		Mtm->nActiveTransactions -= 1;
	}
}

MtmTransState* MtmGetActiveTransaction(MtmL2List* list)
{
	return (MtmTransState*)((char*)list - offsetof(MtmTransState, activeList));
}

/*
 * Prepare transaction for two-phase commit.
 * This code is executed by PRE_PREPARE hook before PREPARE message is sent to replicas by logical replication
 */
static void
MtmPrePrepareTransaction(MtmCurrentTrans* x)
{
	MtmTransState* ts;
	MtmTransMap* tm;
	TransactionId* subxids;
	bool found;
	MTM_TXTRACE(x, "PrePrepareTransaction Start");

	if (!MtmDatabaseId)
		MtmDatabaseId = get_database_oid(MtmDatabaseName, false);

	if (MtmDatabaseId != MyDatabaseId)
		MTM_ELOG(ERROR, "Refusing to work. Multimaster configured to work with database '%s'", MtmDatabaseName);

	if (!x->isDistributed) {
		return;
	}

	if (Mtm->inject2PCError == 1) {
		Mtm->inject2PCError = 0;
		MTM_ELOG(ERROR, "ERROR INJECTION for transaction %s (%llu)", x->gid, (long64)x->xid);
	}
	x->xid = GetCurrentTransactionId();
	Assert(TransactionIdIsValid(x->xid));

	if (!IsBackgroundWorker && Mtm->status != MTM_ONLINE) {
		/* Do not take in account bg-workers which are performing recovery */
		MTM_ELOG(ERROR, "Abort transaction %s (%llu)  because this cluster node is in %s status", x->gid, (long64)x->xid, MtmNodeStatusMnem[Mtm->status]);
	}
	if (TransactionIdIsValid(x->gtid.xid) && BIT_CHECK(Mtm->disabledNodeMask, x->gtid.node-1)) {
		/* Coordinator of transaction is disabled: just abort transaction without any further steps */
		MTM_ELOG(ERROR, "Abort transaction %s (%llu) because it's coordinator %d was disabled", x->gid, (long64)x->xid, x->gtid.node);
	}

	MtmLock(LW_EXCLUSIVE);

	Assert(*x->gid != '\0');
	tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_ENTER, &found);
	if (found && tm->status != TRANSACTION_STATUS_IN_PROGRESS) {
		Assert(tm->status == TRANSACTION_STATUS_ABORTED);
		MtmUnlock();
		MTM_ELOG(ERROR, "Skip already aborted transaction %s (%llu) from node %d", x->gid, (long64)x->xid, x->gtid.node);
	}

	ts = MtmCreateTransState(x);
	/*
	 * Invalid CSN prevent replication of transaction by logical replication
	 */
	ts->snapshot = x->snapshot;
	ts->csn = MtmAssignCSN();
	ts->procno = MyProc->pgprocno;
	ts->votingCompleted = false;
	ts->participantsMask = (((nodemask_t)1 << Mtm->nAllNodes) - 1) & ~Mtm->disabledNodeMask & ~((nodemask_t)1 << (MtmNodeId-1));
	ts->isLocal = x->isReplicated || !x->containsDML || (ts->participantsMask == 0) || MtmEnforceLocalTx;
	ts->nConfigChanges = Mtm->nConfigChanges;
	ts->votedMask = 0;
	ts->nSubxids = xactGetCommittedChildren(&subxids);
	MtmActivateTransaction(ts);
	x->isPrepared = true;
	x->csn = ts->csn;

	tm->state = ts;
	tm->status = TRANSACTION_STATUS_IN_PROGRESS;
	MTM_LOG2("Prepare transaction %s", x->gid);

	Mtm->transCount += 1;
	Mtm->gcCount += 1;

	MtmTransactionListAppend(ts);
	MtmAddSubtransactions(ts, subxids, ts->nSubxids);
	MTM_LOG3("%d: MtmPrePrepareTransaction prepare commit of %d (gtid.xid=%d, gtid.node=%d, CSN=%lld)",
			 MyProcPid, x->xid, ts->gtid.xid, ts->gtid.node, ts->csn);
	MtmUnlock();
	MTM_TXTRACE(x, "PrePrepareTransaction Finish");
}

/*
 * Check heartbeats
 */
bool MtmWatchdog(timestamp_t now)
{
	int i, n = Mtm->nAllNodes;
	bool allAlive = true;
	for (i = 0; i < n; i++) {
		if (i+1 != MtmNodeId && !BIT_CHECK(Mtm->disabledNodeMask, i)) {
			if (Mtm->nodes[i].lastHeartbeat != 0
				&& now > Mtm->nodes[i].lastHeartbeat + MSEC_TO_USEC(MtmHeartbeatRecvTimeout))
			{
				MTM_LOG1("[STATE] Node %i: Disconnect due to heartbeat timeout (%d msec)",
					 i+1, (int)USEC_TO_MSEC(now - Mtm->nodes[i].lastHeartbeat));
				MtmOnNodeDisconnect(i+1);
				allAlive = false;
			}
		}
	}
	return allAlive;
}

/*
 * Mark transaction as precommitted
 */
void MtmPrecommitTransaction(char const* gid)
{
	MtmLock(LW_EXCLUSIVE);
	{
		MtmTransMap* tm = (MtmTransMap*)hash_search(MtmGid2State, gid, HASH_FIND, NULL);
		if (tm == NULL) {
			MtmUnlock();
			MTM_ELOG(WARNING, "MtmPrecommitTransaction: transaction '%s' is not found", gid);
		} else {
			MtmTransState* ts = tm->state;
			// Assert(ts != NULL);
			if (ts == NULL) {
				MTM_ELOG(WARNING, "MtmPrecommitTransaction: transaction '%s' is not yet prepared, status %s", gid, MtmTxnStatusMnem[tm->status]);
				MtmUnlock();
			} else if (ts->status == TRANSACTION_STATUS_IN_PROGRESS) {
				ts->status = TRANSACTION_STATUS_UNKNOWN;
				ts->csn = MtmAssignCSN();
				MtmAdjustSubtransactions(ts);
				MtmUnlock();
				Assert(replorigin_session_origin != InvalidRepOriginId);
				if (!IsTransactionState()) {
					MtmResetTransaction();
					StartTransactionCommand();
					SetPreparedTransactionState(ts->gid, MULTIMASTER_PRECOMMITTED);
					CommitTransactionCommand();
				} else {
					SetPreparedTransactionState(ts->gid, MULTIMASTER_PRECOMMITTED);
				}
				/*
				 * We should send MSG_PRECOMMITTED only after SetPreparedTransactionState()
				 */
				if (Mtm->status != MTM_RECOVERY)
					MtmSend2PCMessage(ts, MSG_PRECOMMITTED);
			} else {
				MTM_ELOG(WARNING, "MtmPrecommitTransaction: transaction '%s' is already in %s state", gid, MtmTxnStatusMnem[ts->status]);
				MtmUnlock();
			}
		}
	}
}





static bool
MtmVotingCompleted(MtmTransState* ts)
{
	nodemask_t liveNodesMask = (((nodemask_t)1 << Mtm->nAllNodes) - 1) & ~Mtm->disabledNodeMask & ~((nodemask_t)1 << (MtmNodeId-1));

	if (!ts->isPrepared) { /* We can not just abort precommitted transactions */
		if (ts->nConfigChanges != Mtm->nConfigChanges)
		{
			MTM_ELOG(WARNING, "Abort transaction %s (%llu) because cluster configuration is changed from %d to %d (old mask %llx, new mask %llx) since transaction start",
				 ts->gid, (long64)ts->xid, ts->nConfigChanges,	Mtm->nConfigChanges, ts->participantsMask, liveNodesMask);
			MtmAbortTransaction(ts);
			return true;
		}
		/* If cluster configuration was not changed, then node mask should not changed as well */
		Assert(ts->participantsMask == liveNodesMask);
	}

	if (ts->votingCompleted) {
		return true;
	}
	if (ts->status == TRANSACTION_STATUS_IN_PROGRESS
		&& (ts->participantsMask & ~Mtm->disabledNodeMask & ~ts->votedMask) == 0) /* all live participants voted */
	{
		if (ts->isPrepared) {
			ts->csn = MtmAssignCSN();
			ts->votingCompleted = true;
			ts->status = TRANSACTION_STATUS_UNKNOWN;
			return true;
		} else {
			MTM_LOG2("Transaction %s is considered as prepared (status=%s participants=%llx disabled=%llx, voted=%llx)",
					 ts->gid, MtmTxnStatusMnem[ts->status], ts->participantsMask, Mtm->disabledNodeMask, ts->votedMask);
			ts->isPrepared = true;
			if (ts->isTwoPhase) {
				ts->votingCompleted = true;
				return true;
			} else if (MtmUseDtm) {
				ts->votedMask = 0;
				Assert(replorigin_session_origin == InvalidRepOriginId);
				MtmUnlock();
				SetPreparedTransactionState(ts->gid, MULTIMASTER_PRECOMMITTED);
				MtmLock(LW_EXCLUSIVE);
				return false;
			} else {
				ts->status = TRANSACTION_STATUS_UNKNOWN;
				ts->votingCompleted = true;
				return true;
			}
		}
	}
	return Mtm->status != MTM_ONLINE /* node is not online */
		|| ts->status == TRANSACTION_STATUS_ABORTED; /* or transaction was aborted */
}

static void
Mtm2PCVoting(MtmCurrentTrans* x, MtmTransState* ts)
{
	int result = 0;
	timestamp_t prepareTime = ts->csn - ts->snapshot;
	timestamp_t timeout = Max(prepareTime + MSEC_TO_USEC(MtmMin2PCTimeout), prepareTime*MtmMax2PCRatio/100);
	timestamp_t start = MtmGetSystemTime();
	timestamp_t deadline = start + timeout;
	timestamp_t now;
	uint32 SaveCancelHoldoffCount = QueryCancelHoldoffCount;

	Assert(ts->csn > ts->snapshot);

	QueryCancelHoldoffCount = 0;

	/* Wait votes from all nodes until: */
	while (!MtmVotingCompleted(ts))
	{
		MtmUnlock();
		MTM_TXTRACE(x, "PostPrepareTransaction WaitLatch Start");
		result = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, MtmHeartbeatRecvTimeout);
		MTM_TXTRACE(x, "PostPrepareTransaction WaitLatch Finish");
		/* Emergency bailout if postmaster has died */
		if (result & WL_POSTMASTER_DEATH) {
			proc_exit(1);
		}
		if (result & WL_LATCH_SET) {
			ResetLatch(&MyProc->procLatch);
		}
		CHECK_FOR_INTERRUPTS();
		now = MtmGetSystemTime();
		MtmLock(LW_EXCLUSIVE);
		if (MtmMin2PCTimeout != 0 && now > deadline) {
			if (ts->isPrepared) {
				MTM_ELOG(LOG, "Distributed transaction %s (%llu) is not committed in %lld msec", ts->gid, (long64)ts->xid, USEC_TO_MSEC(now - start));
			} else {
				MTM_ELOG(WARNING, "Commit of distributed transaction %s (%llu) is canceled because of %lld msec timeout expiration",
					 ts->gid, (long64)ts->xid, USEC_TO_MSEC(timeout));
				MtmAbortTransaction(ts);
				break;
			}
		}
	}
	QueryCancelHoldoffCount = SaveCancelHoldoffCount;

	if (ts->status != TRANSACTION_STATUS_ABORTED && !ts->votingCompleted) {
		if (ts->isPrepared) {
			MTM_ELOG(WARNING, "Commit of distributed transaction %s is suspended because node is switched to %s mode", ts->gid, MtmNodeStatusMnem[Mtm->status]);
			x->isSuspended = true;
		} else {
			if (Mtm->status != MTM_ONLINE) {
				MTM_ELOG(WARNING, "Commit of distributed transaction %s (%llu) is canceled because node is switched to %s mode",
					 ts->gid, (long64)ts->xid, MtmNodeStatusMnem[Mtm->status]);
			} else {
				MTM_ELOG(WARNING, "Commit of distributed transaction %s (%llu) is canceled because cluster configuration was changed",
					 ts->gid, (long64)ts->xid);
			}
			MtmAbortTransaction(ts);
		}
	}
	x->status = ts->status;
	MTM_LOG3("%d: Result of vote: %d", MyProcPid, MtmTxnStatusMnem[ts->status]);
}

static void MtmStopTransaction(void)
{
	if (MtmInsideTransaction) {
		Assert(Mtm->nRunningTransactions > 0);
		Mtm->nRunningTransactions -= 1;
		MtmInsideTransaction = false;
	}
}

static void
MtmPostPrepareTransaction(MtmCurrentTrans* x)
{
	MtmTransState* ts;
	MTM_TXTRACE(x, "PostPrepareTransaction Start");

	if (!x->isDistributed) {
		MTM_TXTRACE(x, "not distributed?");
		return;
	}

	if (Mtm->inject2PCError == 2) {
		Mtm->inject2PCError = 0;
		MTM_ELOG(ERROR, "ERROR INJECTION for transaction %s (%llu)", x->gid, (long64)x->xid);
	}
	MtmLock(LW_EXCLUSIVE);
	ts = (MtmTransState*)hash_search(MtmXid2State, &x->xid, HASH_FIND, NULL);
	Assert(ts != NULL);
	if (!MtmIsCoordinator(ts) || Mtm->status == MTM_RECOVERY) {
		MTM_LOG3("Preparing transaction %d (%s) at %lld", x->xid, x->gid, MtmGetCurrentTime());
		Assert(x->gid[0]);
		ts->votingCompleted = true;
		if (Mtm->status != MTM_RECOVERY/* || Mtm->recoverySlot != MtmReplicationNodeId*/) {
			MtmSend2PCMessage(ts, MSG_PREPARED); /* send notification to coordinator */
			if (!MtmUseDtm) {
				ts->status = TRANSACTION_STATUS_UNKNOWN;
			}
		} else {
			ts->status = TRANSACTION_STATUS_UNKNOWN;
		}
		MtmUnlock();
		MtmResetTransaction();
	} else {
		if (!ts->isLocal)  {
			Mtm2PCVoting(x, ts);

			/* recheck under lock that nothing is changed */
			if (ts->nConfigChanges != Mtm->nConfigChanges)
			{
				MTM_ELOG(WARNING, "XX Abort transaction %s (%llu) because cluster configuration is changed from %d to %d (old mask %llx) since transaction start",
					ts->gid, (long64)ts->xid, ts->nConfigChanges,	Mtm->nConfigChanges, ts->participantsMask);
				MtmAbortTransaction(ts);
				x->status = TRANSACTION_STATUS_ABORTED;
			}
		} else {
			ts->status = TRANSACTION_STATUS_UNKNOWN;
			ts->votingCompleted = true;
		}
		if (x->isTwoPhase) {
			if (x->status == TRANSACTION_STATUS_ABORTED) {
				MTM_ELOG(WARNING, "Prepare of user's 2PC transaction %s (%llu) is aborted by DTM", x->gid, (long64)x->xid);
			}
			MtmStopTransaction();
			MtmResetTransaction();
		}
		MtmUnlock();
	}
	if (Mtm->inject2PCError == 3) {
		Mtm->inject2PCError = 0;
		MTM_ELOG(ERROR, "ERROR INJECTION for transaction %s (%llu)", x->gid, (long64)x->xid);
	}

	MTM_TXTRACE(x, "PostPrepareTransaction Finish");
}

static void
MtmPreCommitPreparedTransaction(MtmCurrentTrans* x)
{
	MtmTransMap* tm;
	MtmTransState* ts;

	MTM_TXTRACE(x, "MtmPreCommitPreparedTransaction Start");

	/* Ignore auto-2PC originated by multimaster */
	if (Mtm->status == MTM_RECOVERY || x->isReplicated || x->isPrepared)
	{
		return;
	}

	MtmLock(LW_EXCLUSIVE);
	tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_FIND, NULL);
	if (tm == NULL)
	{
		MTM_ELOG(WARNING, "Global transaction ID '%s' is not found", x->gid);
	}
	else
	{
		// /*
		//  * Here we should match logic csn shifting logic of MtmEndTransaction.
		//  */
		// if (Mtm->status == MTM_RECOVERY)
		// 	ForceCurrentTransactionTimestamp(x->csn);
		// else
		// 	ForceCurrentTransactionTimestamp(Max(x->csn, tm->state->csn));

		// /* Ignore auto-2PC originated by multimaster */
		// if (Mtm->status == MTM_RECOVERY || x->isReplicated || x->isPrepared)
		// {
		// 	MtmUnlock();
		// 	return;
		// }

		MTM_LOG3("Commit prepared transaction %d with gid='%s'", x->xid, x->gid);
		ts = tm->state;

		Assert(MtmIsCoordinator(ts));
		if (!ts->isLocal) {
			ts->votingCompleted = false;
			ts->votedMask = 0;
			ts->procno = MyProc->pgprocno;
			MTM_LOG2("Coordinator of transaction %s sends MSG_PRECOMMIT", ts->gid);
			Assert(replorigin_session_origin == InvalidRepOriginId);
			MtmUnlock();
			SetPreparedTransactionState(ts->gid, MULTIMASTER_PRECOMMITTED);
			MtmLock(LW_EXCLUSIVE);

			Mtm2PCVoting(x, ts);

			/* recheck under lock that nothing is changed */
			if (ts->nConfigChanges != Mtm->nConfigChanges)
			{
				MTM_ELOG(WARNING, "X Abort transaction %s (%llu) because cluster configuration is changed from %d to %d (old mask %llx) since transaction start",
					ts->gid, (long64)ts->xid, ts->nConfigChanges,	Mtm->nConfigChanges, ts->participantsMask);
				MtmAbortTransaction(ts);
				x->status = TRANSACTION_STATUS_ABORTED;
			}
		} else {
			ts->status = TRANSACTION_STATUS_UNKNOWN;
		}

		x->xid = ts->xid;
		x->csn = ts->csn;
		x->isPrepared = true;
	}
	MtmUnlock();

	MTM_TXTRACE(x, "MtmPreCommitPreparedTransaction Finish");
}

static void
MtmAbortPreparedTransaction(MtmCurrentTrans* x)
{
	MtmTransMap* tm;

	MTM_TXTRACE(x, "MtmAbortPreparedTransaction Start");

	if (x->status != TRANSACTION_STATUS_ABORTED) {
		MtmLock(LW_EXCLUSIVE);
		tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_FIND, NULL);
		if (tm == NULL) {
			MTM_ELOG(WARNING, "Global transaction ID '%s' is not found", x->gid);
		} else {
			MtmTransState* ts = tm->state;
			Assert(ts != NULL);
			MTM_LOG1("Abort prepared transaction %s (%llu)", x->gid, (long64)x->xid);
			MtmAbortTransaction(ts);
			if (ts->isTwoPhase) {
				MtmDeactivateTransaction(ts);
			}
		}
		MtmUnlock();
		x->status = TRANSACTION_STATUS_ABORTED;
	} else {
		MTM_LOG1("Transaction %s (%llu) is already aborted", x->gid, (long64)x->xid);
	}

	MTM_TXTRACE(x, "MtmAbortPreparedTransaction Finish");
}

static void
MtmLogAbortLogicalMessage(int nodeId, char const* gid)
{
	MtmAbortLogicalMessage msg;
	lsn_t lsn;
	strcpy(msg.gid, gid);
	msg.origin_node = nodeId;
	msg.origin_lsn = replorigin_session_origin_lsn;
	lsn = LogLogicalMessage("A", (char*)&msg, sizeof msg, false);
	XLogFlush(lsn);
	MTM_LOG1("MtmLogAbortLogicalMessage node=%d transaction=%s lsn=%llx", nodeId, gid, lsn);
}


static void
MtmEndTransaction(MtmCurrentTrans* x, bool commit)
{
	MTM_LOG3("%d: End transaction %lld, prepared=%d, replicated=%d, distributed=%d, 2pc=%d, gid=%s -> %s, LSN %lld",
			 MyProcPid, (long64)x->xid, x->isPrepared, x->isReplicated, x->isDistributed, x->isTwoPhase, x->gid, commit ? "commit" : "abort", (long64)GetXLogInsertRecPtr());
	commit &= (x->status != TRANSACTION_STATUS_ABORTED);

	MTM_TXTRACE(x, "MtmEndTransaction Start (c=%d)", commit);

	MtmLock(LW_EXCLUSIVE);

	MtmStopTransaction();

	if (x->isDistributed && (x->isPrepared || x->isReplicated) && !x->isTwoPhase) {
		MtmTransState* ts = NULL;
		if (x->isPrepared) {
			ts = (MtmTransState*)hash_search(MtmXid2State, &x->xid, HASH_FIND, NULL);
			Assert(ts != NULL);
			Assert(strcmp(x->gid, ts->gid) == 0);
		} else if (x->gid[0]) {
			MtmTransMap* tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_FIND, NULL);
			if (tm != NULL) {
				ts = tm->state;
			} else {
				MTM_LOG1("%d: GID %s not found", MyProcPid, x->gid);
			}
		}
		if (ts != NULL) {
			if (*ts->gid)
				MTM_LOG2("TRANSLOG: %s transaction gid=%s xid=%d node=%d dxid=%d status %s",
						 (commit ? "commit" : "rollback"), ts->gid, ts->xid, ts->gtid.node, ts->gtid.xid, MtmTxnStatusMnem[ts->status]);
			if (commit) {
				if (!(ts->status == TRANSACTION_STATUS_UNKNOWN
					  || (ts->status == TRANSACTION_STATUS_IN_PROGRESS && Mtm->status <= MTM_RECOVERY)))
				{
					MtmUnlock();
					MTM_ELOG(ERROR, "Attempt to commit %s transaction %s (%llu)",
						 MtmTxnStatusMnem[ts->status], ts->gid, (long64)ts->xid);
				}
				if (x->csn > ts->csn || Mtm->status == MTM_RECOVERY) {
					Assert(x->csn != INVALID_CSN);
					ts->csn = x->csn;
					MtmSyncClock(ts->csn);
				}
				if (ts->isLocal && !x->isReplicated)
				{
					ts->csn = MtmAssignCSN();
				}
				Mtm->lastCsn = ts->csn;
				ts->status = TRANSACTION_STATUS_COMMITTED;
				MtmAdjustSubtransactions(ts);
			} else {
				MTM_LOG1("%d: abort transaction %s (%llu) is called from MtmEndTransaction", MyProcPid, x->gid, (long64)x->xid);
				MtmAbortTransaction(ts);
			}
			MtmDeactivateTransaction(ts);
		}
		x->isActive = false;

		if (!commit && x->isReplicated && TransactionIdIsValid(x->gtid.xid)) {
			Assert(Mtm->status != MTM_RECOVERY || Mtm->recoverySlot != MtmNodeId);
			/*
			 * Send notification only if ABORT happens during transaction processing at replicas,
			 * do not send notification if ABORT is received from master
			 */
			MTM_LOG1("%d: send ABORT notification for transaction %s (%llu) local xid=%llu to coordinator %d",
					 MyProcPid, x->gid, (long64)x->gtid.xid, (long64)x->xid, x->gtid.node);
			if (ts == NULL) {
				bool found;
				Assert(TransactionIdIsValid(x->xid));
				ts = (MtmTransState*)hash_search(MtmXid2State, &x->xid, HASH_ENTER, &found);
				if (!found) {
					ts->isEnqueued = false;
					ts->isActive = false;
				}
				ts->status = TRANSACTION_STATUS_ABORTED;
				ts->isLocal = true;
				ts->isPrepared = false;
				ts->isPinned = false;
				ts->snapshot = x->snapshot;
				ts->isTwoPhase = x->isTwoPhase;
				ts->csn = MtmAssignCSN();
				ts->gtid = x->gtid;
				ts->nSubxids = 0;
				ts->votingCompleted = true;
				strcpy(ts->gid, x->gid);
				MtmTransactionListAppend(ts);
				if (*x->gid) {
					replorigin_session_origin_lsn = INVALID_LSN;
					MTM_TXTRACE(x, "MtmEndTransaction/MtmLogAbortLogicalMessage");
					MtmLogAbortLogicalMessage(MtmNodeId, x->gid);
				}
				MtmDeactivateTransaction(ts);
				x->isActive = false;
			}
			MtmSend2PCMessage(ts, MSG_ABORTED); /* send notification to coordinator */
#if 0
		} else if (x->status == TRANSACTION_STATUS_ABORTED && x->isReplicated && !x->isPrepared) {
			hash_search(MtmXid2State, &x->xid, HASH_REMOVE, NULL);
#endif
		}
		Assert(!x->isActive);
	}
	MtmUnlock();

	MTM_TXTRACE(x, "MtmEndTransaction Finish");

	MtmResetTransaction();
	// if (MtmClusterLocked) {
	// 	MtmUnlockCluster();
	// }
}

/*
 * Initialize message
 */
void MtmInitMessage(MtmArbiterMessage* msg, MtmMessageCode code)
{
	msg->code = code;
	msg->disabledNodeMask = Mtm->disabledNodeMask;
	msg->connectivityMask = SELF_CONNECTIVITY_MASK;
	msg->oldestSnapshot = Mtm->nodes[MtmNodeId-1].oldestSnapshot;
	msg->lockReq = Mtm->originLockNodeMask != 0;
	msg->locked = (Mtm->originLockNodeMask|Mtm->inducedLockNodeMask) != 0;
}


/*
 * Send arbiter's message
 */
void MtmSendMessage(MtmArbiterMessage* msg)
{
	SpinLockAcquire(&Mtm->queueSpinlock);
	{
		MtmMessageQueue* mq = Mtm->freeQueue;
		MtmMessageQueue* sendQueue = Mtm->sendQueue;
		if (mq == NULL) {
			mq = (MtmMessageQueue*)ShmemAlloc(sizeof(MtmMessageQueue));
			if (mq == NULL) {
				elog(PANIC, "Failed to allocate shared memory for message queue");
			}
		} else {
			Mtm->freeQueue = mq->next;
		}
		mq->msg = *msg;
		mq->next = sendQueue;
		Mtm->sendQueue = mq;
		if (sendQueue == NULL) {
			/* signal semaphore only once for the whole list */
			PGSemaphoreUnlock(&Mtm->sendSemaphore);
		}
	}
	SpinLockRelease(&Mtm->queueSpinlock);
}

/*
 * Send arbiter's 2PC message. Right now only responses to coordinates are
 * sent through arbiter. Broadcasts from coordinator to noes are done
 * using logical decoding.
 */
void MtmSend2PCMessage(MtmTransState* ts, MtmMessageCode cmd)
{
	MtmArbiterMessage msg;
	MtmInitMessage(&msg, cmd);
	msg.sxid = ts->xid;
	msg.csn	 = ts->csn;
	memcpy(msg.gid, ts->gid, MULTIMASTER_MAX_GID_SIZE);

	Assert(!MtmIsCoordinator(ts));	/* All broadcasts are now done through logical decoding */
	if (!BIT_CHECK(Mtm->disabledNodeMask, ts->gtid.node-1))
	{
		MTM_TXTRACE(ts, "MtmSend2PCMessage sending %s message to node %d", MtmMessageKindMnem[cmd], ts->gtid.node);
		msg.node = ts->gtid.node;
		msg.dxid = ts->gtid.xid;
		MtmSendMessage(&msg);
	}
}

/*
 * Broadcast poll state message to all nodes.
 * This function is used to gather information about state of prepared transaction
 * at node startup or after crash of some node.
 */
static void MtmBroadcastPollMessage(MtmTransState* ts)
{
	int i;
	MtmArbiterMessage msg;

	MTM_LOG1("MtmBroadcastPollMessage: %s %lld", ts->gid, ts->participantsMask);
	MtmInitMessage(&msg, MSG_POLL_REQUEST);
	memcpy(msg.gid, ts->gid, MULTIMASTER_MAX_GID_SIZE);
	ts->votedMask = 0;

	for (i = 0; i < Mtm->nAllNodes; i++)
	{
		if (BIT_CHECK(ts->participantsMask, i))
		{
			msg.node = i+1;
			MTM_LOG3("Send request for transaction %s to node %d", msg.gid, msg.node);
			MtmSendMessage(&msg);
		}
	}
}

/*
 * Restore state of recovered prepared transaction in memory.
 * This function is called at system startup to make it possible to
 * handle this prepared transactions in normal way.
 */
static void	MtmLoadPreparedTransactions(void)
{
	PreparedTransaction pxacts;
	int n = GetPreparedTransactions(&pxacts);
	int i;

	for (i = 0; i < n; i++) {
		bool found;
		char const* gid = pxacts[i].gid;
		MtmTransMap* tm = (MtmTransMap*)hash_search(MtmGid2State, gid, HASH_ENTER, &found);
		if (!found || tm->state == NULL) {
			TransactionId xid = GetNewTransactionId(false);
			MtmTransState* ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_ENTER, &found);
			MTM_LOG1("Recover prepared transaction %s (%llu) state=%s", gid, (long64)xid, pxacts[i].state_3pc);
			MyPgXact->xid = InvalidTransactionId; /* dirty hack:((( */
			Assert(!found);
			ts->isEnqueued = false;
			ts->isActive = false;
			MtmActivateTransaction(ts);
			ts->status = strcmp(pxacts[i].state_3pc, MULTIMASTER_PRECOMMITTED) == 0 ? TRANSACTION_STATUS_UNKNOWN : TRANSACTION_STATUS_IN_PROGRESS;
			ts->isLocal = true;
			ts->isPrepared = true;
			ts->isPinned = false;
			ts->snapshot = INVALID_CSN;
			ts->isTwoPhase = false;
			ts->csn = 0; /* should be replaced with real CSN by poll result */
			ts->gtid.node = MtmNodeId;
			ts->gtid.xid = xid;
			ts->nSubxids = 0;
			ts->votingCompleted = true;
			ts->participantsMask = (((nodemask_t)1 << Mtm->nAllNodes) - 1) & ~((nodemask_t)1 << (MtmNodeId-1));
			ts->nConfigChanges = Mtm->nConfigChanges;
			ts->votedMask = 0;
			strcpy(ts->gid, gid);
			MtmTransactionListAppend(ts);
			tm->status = ts->status;
			tm->state = ts;
			MtmBroadcastPollMessage(ts);
		}
	}
	MTM_LOG1("Recover %d prepared transactions", n);
	if (pxacts) {
		pfree(pxacts);
	}
}

static void MtmDropSlot(int nodeId)
{
	if (MtmTryLockNode(nodeId, LW_EXCLUSIVE))
	{
		MTM_ELOG(INFO, "Drop replication slot for node %d", nodeId);
		ReplicationSlotDrop(psprintf(MULTIMASTER_SLOT_PATTERN, nodeId));
		MtmUnlockNode(nodeId);
	} else {
		MTM_ELOG(WARNING, "Failed to drop replication slot for node %d", nodeId);
	}
	MtmLock(LW_EXCLUSIVE);
	BIT_SET(Mtm->stalledNodeMask, nodeId-1);
	BIT_SET(Mtm->stoppedNodeMask, nodeId-1); /* stalled node can not be automatically recovered */
	MtmUnlock();
}

/*
 * Prepare context for applying transaction at replica.
 * It also checks that coordinator of transaction is not disabled and all live nodes are participated in this transaction.
 */
void MtmJoinTransaction(GlobalTransactionId* gtid, csn_t globalSnapshot, nodemask_t participantsMask)
{
	nodemask_t liveMask;

	MtmTx.gtid = *gtid;
	MtmTx.xid = GetCurrentTransactionId();
	MtmTx.isReplicated = true;
	MtmTx.isDistributed = true;
	MtmTx.containsDML = true;

	if (globalSnapshot != INVALID_CSN) {
		MtmLock(LW_EXCLUSIVE);

		liveMask = (((nodemask_t)1 << Mtm->nAllNodes) - 1) & ~Mtm->disabledNodeMask;
		BIT_SET(participantsMask, gtid->node-1);
		if (liveMask & ~participantsMask) {
			MtmUnlock();
			MTM_ELOG(ERROR, "Ignore transaction %llu from node %d because some of live nodes (%llx) are not participated in it (%llx)",
				 (long64)gtid->xid, gtid->node, liveMask, participantsMask);
		}

		MtmSyncClock(globalSnapshot);
		MtmTx.snapshot = globalSnapshot;
		if (Mtm->status != MTM_RECOVERY) {
			MtmTransState* ts = MtmCreateTransState(&MtmTx); /* we need local->remote xid mapping for deadlock detection */
			MtmActivateTransaction(ts);
		}
		MtmUnlock();
	} else {
		globalSnapshot = MtmTx.snapshot;
	}


	// if (!TransactionIdIsValid(gtid->xid) && Mtm->status != MTM_RECOVERY)
	// {
	// 	/* In case of recovery InvalidTransactionId is passed */
	// 	MtmStateProcessEvent(MTM_RECOVERY_START1);
	// }
	// else if (Mtm->status == MTM_RECOVERY)
	// {
	// 	/* When recovery is completed we get normal transaction ID and switch to normal mode */
	// 	MtmStateProcessEvent(MTM_RECOVERY_FINISH1);
	// }
}

void  MtmSetCurrentTransactionGID(char const* gid)
{
	MTM_LOG3("Set current transaction xid=%d GID %s", MtmTx.xid, gid);
	strcpy(MtmTx.gid, gid);
	MtmTx.isDistributed = true;
	MtmTx.isReplicated = true;
}

TransactionId MtmGetCurrentTransactionId(void)
{
	return MtmTx.xid;
}

XidStatus MtmGetCurrentTransactionStatus(void)
{
	return MtmTx.status;
}

/*
 * Perform atomic exchange of global transaction status.
 * The problem is that because of concurrent applying transactions at replica by multiple
 * threads we can proceed ABORT request before PREPARE - when transaction is not yet
 * applied at this node and there is MtmTransState associated with this transactions.
 * We remember information about status of this transaction in MtmTransMap.
 */
XidStatus MtmExchangeGlobalTransactionStatus(char const* gid, XidStatus new_status)
{
	MtmTransMap* tm;
	bool found;
	XidStatus old_status = TRANSACTION_STATUS_IN_PROGRESS;

	Assert(gid[0]);
	MtmLock(LW_EXCLUSIVE);
	tm = (MtmTransMap*)hash_search(MtmGid2State, gid, HASH_ENTER, &found);
	if (found) {
		old_status = tm->status;
		if (old_status != TRANSACTION_STATUS_ABORTED) {
			tm->status = new_status;
		}
		if (tm->state != NULL && old_status == TRANSACTION_STATUS_IN_PROGRESS) {
			/* Return UNKNOWN to mark that transaction was prepared */
			if (new_status != TRANSACTION_STATUS_UNKNOWN) {
				MTM_LOG1("Change status of in-progress transaction %s to %s", gid, MtmTxnStatusMnem[new_status]);
			}
			old_status = TRANSACTION_STATUS_UNKNOWN;
		}
	} else {
		MTM_LOG2("Set status of unknown transaction %s to %s", gid, MtmTxnStatusMnem[new_status]);
		tm->state = NULL;
		tm->status = new_status;
	}
	MtmUnlock();
	return old_status;
}

void  MtmSetCurrentTransactionCSN(csn_t csn)
{
	MTM_LOG3("Set current transaction CSN %lld", csn);
	MtmTx.csn = csn;
	MtmTx.isDistributed = true;
	MtmTx.isReplicated = true;
}


csn_t MtmGetTransactionCSN(TransactionId xid)
{
	MtmTransState* ts;
	csn_t csn;
	MtmLock(LW_SHARED);
	ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	csn = ts ? ts->csn : INVALID_CSN;
	MtmUnlock();
	return csn;
}

/*
 * Wakeup coordinator's backend when voting is completed
 */
void MtmWakeUpBackend(MtmTransState* ts)
{
	if (!ts->votingCompleted) {
		MTM_TXTRACE(ts, "MtmWakeUpBackend");
		MTM_LOG3("Wakeup backed procno=%d, pid=%d", ts->procno, ProcGlobal->allProcs[ts->procno].pid);
		ts->votingCompleted = true;
		SetLatch(&ProcGlobal->allProcs[ts->procno].procLatch);
	}
}


/*
 * Abort the transaction if it is not yet aborted
 */
void MtmAbortTransaction(MtmTransState* ts)
{
	Assert(MtmLockCount != 0); /* should be invoked with exclusive lock */
	if (ts->status != TRANSACTION_STATUS_ABORTED) {
		if (ts->status == TRANSACTION_STATUS_COMMITTED) {
			MTM_ELOG(LOG, "Attempt to rollback already committed transaction %s (%llu)", ts->gid, (long64)ts->xid);
		} else {
			MTM_LOG1("Rollback active transaction %s (%llu) %d:%llu status %s", ts->gid, (long64)ts->xid, ts->gtid.node, (long64)ts->gtid.xid, MtmTxnStatusMnem[ts->status]);
			ts->status = TRANSACTION_STATUS_ABORTED;
			MtmAdjustSubtransactions(ts);
		}
	}
}

/*
 * -------------------------------------------
 * HA functions
 * -------------------------------------------
 */

/*
 * Handle critical errors while applying transaction at replica.
 * Such errors should cause shutdown of this cluster node to allow other nodes to continue serving client requests.
 * Other error will be just reported and ignored
 */
void MtmHandleApplyError(void)
{
	ErrorData *edata = CopyErrorData();
	MtmLockCount = 0; /* LWLocks will be released by AbortTransaction, we just need to clear owr MtmLockCount */
	switch (edata->sqlerrcode) {
		case ERRCODE_DISK_FULL:
		case ERRCODE_INSUFFICIENT_RESOURCES:
		case ERRCODE_IO_ERROR:
		case ERRCODE_DATA_CORRUPTED:
		case ERRCODE_INDEX_CORRUPTED:
		  /* Should we really treate this errors as fatal?
		case ERRCODE_SYSTEM_ERROR:
		case ERRCODE_INTERNAL_ERROR:
		case ERRCODE_OUT_OF_MEMORY:
		  */
			MtmStateProcessEvent(MTM_NONRECOVERABLE_ERROR);
	}
	FreeErrorData(edata);
}

/**
 * Check status of all prepared transactions with coordinator at disabled node.
 * Actually, if node is precommitted (state == UNKNOWN) at any of nodes, then is is prepared at all nodes and so can be committed.
 * But if coordinator of transaction is crashed, we made a decision about transaction commit only if transaction is precommitted at ALL live nodes.
 * The reason is that we want to avoid extra polling to obtain maximum CSN from all nodes to assign it to committed transaction.
 * Called only from MtmDisableNode and in major mode.
 *
 * commitPrecommited is used when nnodes=2 and we are switching to major/referee mode.
 */
void MtmPollStatusOfPreparedTransactionsForDisabledNode(int disabledNodeId, bool commitPrecommited)
{
	MtmTransState *ts;
	for (ts = Mtm->transListHead; ts != NULL; ts = ts->next) {
		if (TransactionIdIsValid(ts->gtid.xid)
			&& ts->gtid.node == disabledNodeId
			&& ts->votingCompleted
			&& (ts->status == TRANSACTION_STATUS_UNKNOWN || ts->status == TRANSACTION_STATUS_IN_PROGRESS))
		{
			Assert(ts->gid[0]);
			if (ts->status == TRANSACTION_STATUS_IN_PROGRESS) {
				MTM_ELOG(LOG, "Abort transaction %s because its coordinator is disabled and it is not prepared at node %d", ts->gid, MtmNodeId);
				TXFINISH("%s ABORT, PollStatusOfPrepared", ts->gid);
				MtmFinishPreparedTransaction(ts, false);
			} else {
				if (commitPrecommited)
				{
					TXFINISH("%s COMMIT, PollStatusOfPrepared", ts->gid);
					MtmFinishPreparedTransaction(ts, true);
				}
				else
				{
					MTM_LOG1("Poll state of transaction %s (%llu)", ts->gid, (long64)ts->xid);
					MtmBroadcastPollMessage(ts);
				}
			}
		} else {
			MTM_LOG2("Skip transaction %s (%llu) with status %s gtid.node=%d gtid.xid=%llu votedMask=%llx",
					 ts->gid, (long64)ts->xid, MtmTxnStatusMnem[ts->status], ts->gtid.node, (long64)ts->gtid.xid, ts->votedMask);
		}
	}
}

/*
 * Poll status of all active prepared transaction.
 * This function is called before start of recovery to prevent blocking of recovery process by some
 * prepared transaction which is not recovered
 */
void
MtmPollStatusOfPreparedTransactions(bool majorMode)
{
	MtmTransState *ts;
	for (ts = Mtm->transListHead; ts != NULL; ts = ts->next) {
		if (TransactionIdIsValid(ts->gtid.xid)
			&& ts->votingCompleted /* If voting is not yet completed, then there is some backend coordinating this transaction */
			&& (ts->status == TRANSACTION_STATUS_UNKNOWN || ts->status == TRANSACTION_STATUS_IN_PROGRESS))
		{
			Assert(ts->gid[0]);

			if (majorMode)
			{
				MtmFinishPreparedTransaction(ts, false);
			}
			else
			{
				MTM_LOG1("Poll state of transaction %s (%llu) from node %d", ts->gid, (long64)ts->xid, ts->gtid.node);
				MtmBroadcastPollMessage(ts);
			}
		} else {
			MTM_LOG2("Skip prepared transaction %s (%d) with status %s gtid.node=%d gtid.xid=%llu votedMask=%llx",
					 ts->gid, (long64)ts->xid, MtmTxnStatusMnem[ts->status], ts->gtid.node, (long64)ts->gtid.xid, ts->votedMask);
		}
	}
}



/**
 * Check state of replication slots. If some of them are too much lag behind wal, then drop this slots to avoid
 * WAL overflow
 */
static void
MtmCheckSlots()
{
	if (MtmMaxRecoveryLag != 0 && Mtm->disabledNodeMask != 0)
	{
		int i;
		for (i = 0; i < max_replication_slots; i++) {
			ReplicationSlot* slot = &ReplicationSlotCtl->replication_slots[i];
			int nodeId;
			if (slot->in_use
				&& sscanf(slot->data.name.data, MULTIMASTER_SLOT_PATTERN, &nodeId) == 1
				&& BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)
				&& slot->data.confirmed_flush + (long64) MtmMaxRecoveryLag * 1024 < GetXLogInsertRecPtr()
				&& slot->data.confirmed_flush != 0)
			{
				MTM_ELOG(WARNING, "Drop slot for node %d which lag %lld B is larger than threshold %d kB",
					 nodeId,
					 (long64)(GetXLogInsertRecPtr() - slot->data.restart_lsn),
					 MtmMaxRecoveryLag);
				MtmDropSlot(nodeId);
			}
		}
	}
}

/*
 * Get lag between replication slot position (dsata proceeded by WAL sender) and current position in WAL
 */
static int64 MtmGetSlotLag(int nodeId)
{
	int i;
	for (i = 0; i < max_replication_slots; i++) {
		ReplicationSlot* slot = &ReplicationSlotCtl->replication_slots[i];
		int node;
		if (slot->in_use
			&& sscanf(slot->data.name.data, MULTIMASTER_SLOT_PATTERN, &node) == 1
			&& node == nodeId)
		{
			return GetXLogInsertRecPtr() - slot->data.confirmed_flush;
		}
	}
	return -1;
}


/*
 * This function is called by WAL sender when start sending new transaction.
 * It returns true if specified node is in recovery mode. In this case we should send to it all transactions from WAL,
 * not only coordinated by self node as in normal mode.
 */
bool MtmIsRecoveredNode(int nodeId)
{
	if (BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)) {
		if (!MtmIsRecoverySession) {
			MtmDeepUnlock();
			MTM_ELOG(ERROR, "Node %d is marked as disabled but is not in recovery mode", nodeId);
		}
		return true;
	} else {
		return false;
	}
}

/*
 * Check if wal sender replayed all transactions from WAL log.
 * It can never happen if there are many active transactions.
 * In this case we wait until gap between sent and current position in the
 * WAL becomes smaller than threshold value MtmMinRecoveryLag and
 * after it prohibit start of new transactions until WAL is completely replayed.
 */
void MtmCheckRecoveryCaughtUp(int nodeId, lsn_t slotLSN)
{
	MtmLock(LW_EXCLUSIVE);
	if (MtmIsRecoveredNode(nodeId)) {
		lsn_t walLSN = GetXLogInsertRecPtr();
		if (!BIT_CHECK(Mtm->originLockNodeMask, nodeId-1)
			&& slotLSN + (long64) MtmMinRecoveryLag * 1024 > walLSN)
		{
			/*
			 * Wal sender almost caught up.
			 * Lock cluster preventing new transaction to start until wal is completely replayed.
			 * We have to maintain two bitmasks: one is marking wal sender, another - correspondent nodes.
			 * Is there some better way to establish mapping between nodes ad WAL-seconder?
			 */
			MTM_LOG1("Node %d is almost caught-up: slot position %llx, WAL position %llx, active transactions %d",
				 nodeId, slotLSN, walLSN, Mtm->nActiveTransactions);

			MTM_LOG1("[LOCK] set lock on MtmCheckRecoveryCaughtUp");
			BIT_SET(Mtm->originLockNodeMask, nodeId-1); // XXXX: log that
		} else {
			MTM_LOG2("Continue recovery of node %d, slot position %llx, WAL position %llx,"
					 " WAL sender position %llx, lockers %llx, active transactions %d", nodeId, slotLSN,
					 walLSN, MyWalSnd->sentPtr, Mtm->orinLockNodeMask, Mtm->nActiveTransactions);
		}
	}
	MtmUnlock();
}

/*
 * Notification about node recovery completion.
 * If recovery is in progress and WAL sender replays all records in WAL,
 * then enable recovered node and send notification to it about end of recovery.
 */
bool MtmRecoveryCaughtUp(int nodeId, lsn_t walEndPtr)
{
	bool caughtUp = false;
	MtmLock(LW_EXCLUSIVE);
	if (MtmIsRecoveredNode(nodeId) && Mtm->nActiveTransactions == 0) {
		MtmStateProcessNeighborEvent(nodeId, MTM_NEIGHBOR_RECOVERY_CAUGHTUP);
		caughtUp = true;
		MtmIsRecoverySession = false;
	}
	MtmUnlock();
	return caughtUp;
}

// /*
//  * Prevent start of any new transactions at this node
//  */
// static void
// MtmLockCluster(void)
// {
// 	timestamp_t delay = MIN_WAIT_TIMEOUT;
// 	if (MtmClusterLocked) {
// 		MtmUnlockCluster();
// 	}
// 	MtmLock(LW_EXCLUSIVE);
// 	if (BIT_CHECK(Mtm->originLockNodeMask, MtmNodeId-1)) {
// 		MtmUnlock();
// 		elog(ERROR, "There is already pending exclusive lock");
// 	}
// 	BIT_SET(Mtm->originLockNodeMask, MtmNodeId-1);
// 	MtmClusterLocked = true;
// 	MTM_LOG1("Transaction %lld tries to lock cluster at %lld, running transactions=%lld",
// 			 (long64)MtmTx.xid, MtmGetCurrentTime(), (long64)Mtm->nRunningTransactions);
// 	/* Wait until everything is locked */
// 	while (Mtm->nRunningTransactions != 1 /* I am one */
// 		   || ((((nodemask_t)1 << Mtm->nAllNodes)-1) & ~(Mtm->currentLockNodeMask|Mtm->originLockNodeMask) & ~Mtm->disabledNodeMask) != 0)
// 	{
// 		MtmUnlock();
// 		MtmSleep(delay);
// 		if (delay*2 <= MAX_WAIT_TIMEOUT) {
// 			delay *= 2;
// 		}
// 		MtmLock(LW_EXCLUSIVE);
// 	}
// 	MTM_LOG1("Transaction %lld locked cluster at %lld, LSN %lld, active transactions=%lld",
// 			 (long64)MtmTx.xid, MtmGetCurrentTime(), (long64)GetXLogInsertRecPtr(), (long64)Mtm->nRunningTransactions);
// 	MtmUnlock();
// }

// /*
//  * Remove global cluster lock set by MtmLockCluster
//  */
// static void
// MtmUnlockCluster(void)
// {
// 	MtmLock(LW_EXCLUSIVE);
// 	MTM_LOG1("Transaction %lld unlock cluster at %lld status %s LSN %lld", (long64)MtmTx.xid, MtmGetCurrentTime(),	MtmTxnStatusMnem[MtmTx.status], (long64)GetXLogInsertRecPtr());
// 	BIT_CLEAR(Mtm->originLockNodeMask, MtmNodeId-1);
// 	MtmClusterLocked = false;
// 	MtmUnlock();
// }

// /*
//  * If there are recovering nodes which are catching-up WAL, check the status and prevent new transaction from commit to give
//  * WAL-sender a chance to catch-up WAL, completely synchronize replica and switch it to normal mode.
//  * This function is called before transaction prepare with multimaster lock set.
//  */
// static void
// MtmCheckClusterLock()
// {
// 	timestamp_t delay = MIN_WAIT_TIMEOUT;
// 	while (Mtm->originLockNodeMask | Mtm->inducedLockNodeMask) {
// 		/* some "almost cautch-up" wal-senders are still working. */
// 		/* Do not start new transactions until them are completed. */
// 		MtmUnlock();
// 		MtmSleep(delay);
// 		if (delay*2 <= MAX_WAIT_TIMEOUT) {
// 			delay *= 2;
// 		}
// 		MtmLock(LW_EXCLUSIVE);
// 	}
// }

int MtmGetNumberOfVotingNodes()
{
	int i;
	int nVotingNodes = Mtm->nAllNodes;
	nodemask_t deadNodeMask = Mtm->deadNodeMask;
	for (i = 0; deadNodeMask != 0; i++) {
		if (BIT_CHECK(deadNodeMask, i)) {
			nVotingNodes -= 1;
			BIT_CLEAR(deadNodeMask, i);
		}
	}
	return nVotingNodes;
}

/*
 * -------------------------------------------
 * Node initialization
 * -------------------------------------------
 */

/*
 * Initialize Xid2State hash table to obtain status of transaction by its local XID.
 * Size of this hash table should be limited by MtmAdjustOldestXid function which performs cleanup
 * of transaction list and from the list and from the hash table transactions which XIDs are not used in any snapshot at any node
 */
static HTAB*
MtmCreateXidMap(void)
{
	HASHCTL info;
	HTAB* htab;
	Assert(MtmMaxNodes > 0);
	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(TransactionId);
	info.entrysize = sizeof(MtmTransState) + (MtmMaxNodes-1)*sizeof(TransactionId);
	htab = ShmemInitHash(
		"MtmXid2State",
		MTM_HASH_SIZE, MTM_HASH_SIZE,
		&info,
		HASH_ELEM | HASH_BLOBS
	);
	return htab;
}

/*
 * Initialize Gid2State hash table to obtain status of transaction by GID.
 * Size of this hash table should be limited by MtmAdjustOldestXid function which performs cleanup
 * of transaction list and from the list and from the hash table transactions which XIDs are not used in any snapshot at any node
 */
static HTAB*
MtmCreateGidMap(void)
{
	HASHCTL info;
	HTAB* htab;
	memset(&info, 0, sizeof(info));
	info.keysize = MULTIMASTER_MAX_GID_SIZE;
	info.entrysize = sizeof(MtmTransMap);
	htab = ShmemInitHash(
		"MtmGid2State",
		MTM_MAP_SIZE, MTM_MAP_SIZE,
		&info,
		HASH_ELEM
	);
	return htab;
}

/*
 * Initialize hash table used to mark local (not distributed) tables
 */
static HTAB*
MtmCreateLocalTableMap(void)
{
	HASHCTL info;
	HTAB* htab;
	memset(&info, 0, sizeof(info));
	info.entrysize = info.keysize = sizeof(Oid);
	htab = ShmemInitHash(
		"MtmLocalTables",
		MULTIMASTER_MAX_LOCAL_TABLES, MULTIMASTER_MAX_LOCAL_TABLES,
		&info,
		HASH_ELEM | HASH_BLOBS
	);
	return htab;
}

void MtmMakeRelationLocal(Oid relid)
{
	if (OidIsValid(relid)) {
		MtmLock(LW_EXCLUSIVE);
		hash_search(MtmLocalTables, &relid, HASH_ENTER, NULL);
		MtmUnlock();
	}
}


void MtmMakeTableLocal(char const* schema, char const* name)
{
	RangeVar* rv = makeRangeVar((char*)schema, (char*)name, -1);
	Oid relid = RangeVarGetRelid(rv, NoLock, true);
	MtmMakeRelationLocal(relid);
}


typedef struct {
	NameData schema;
	NameData name;
} MtmLocalTablesTuple;

static void MtmLoadLocalTables(void)
{
	RangeVar	   *rv;
	Relation		rel;
	SysScanDesc		scan;
	HeapTuple		tuple;

	Assert(IsTransactionState());

	rv = makeRangeVar(MULTIMASTER_SCHEMA_NAME, MULTIMASTER_LOCAL_TABLES_TABLE, -1);
	rel = heap_openrv_extended(rv, RowExclusiveLock, true);
	if (rel != NULL) {
		scan = systable_beginscan(rel, 0, true, NULL, 0, NULL);

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			MtmLocalTablesTuple	*t = (MtmLocalTablesTuple*) GETSTRUCT(tuple);
			MtmMakeTableLocal(NameStr(t->schema), NameStr(t->name));
		}

		systable_endscan(scan);
		heap_close(rel, RowExclusiveLock);
	}
}

/*
 * Multimaster control file is used to prevent erroneous inclusion of node in the cluster.
 * It contains cluster name (any user defined identifier) and node id.
 * In case of creating new cluster node using pg_basebackup this file is copied together will
 * all other PostgreSQL files and so new node will know ID of the cluster node from which it
 * is cloned. It is necessary to complete synchronization of new node with the rest of the cluster.
 */
static void MtmCheckControlFile(void)
{
	char controlFilePath[MAXPGPATH];
	char buf[MULTIMASTER_MAX_CTL_STR_SIZE];
	FILE* f;
	snprintf(controlFilePath, MAXPGPATH, "%s/global/mmts_control", DataDir);
	f = fopen(controlFilePath, "r");
	if (f != NULL && fgets(buf, sizeof buf, f)) {
		char* sep = strchr(buf, ':');
		if (sep == NULL) {
			MTM_ELOG(FATAL, "File mmts_control doesn't contain cluster name");
		}
		*sep = '\0';
		if (strcmp(buf, MtmClusterName) != 0) {
			MTM_ELOG(FATAL, "Database belongs to some other cluster %s rather than %s", buf, MtmClusterName);
		}
		if (sscanf(sep+1, "%d", &Mtm->donorNodeId) != 1) {
			MTM_ELOG(FATAL, "File mmts_control doesn't contain node id");
		}
		fclose(f);
	} else {
		if (f != NULL) {
			fclose(f);
		}
		f = fopen(controlFilePath, "w");
		if (f == NULL) {
			MTM_ELOG(FATAL, "Failed to create mmts_control file: %m");
		}
		Mtm->donorNodeId = MtmNodeId;
		fprintf(f, "%s:%d\n", MtmClusterName, Mtm->donorNodeId);
		fclose(f);
	}
}

/*
 * Perform initialization of multimaster state.
 * This function is called from shared memory startup hook (after completion of initialization of shared memory)
 */
static void MtmInitialize()
{
	bool found;
	int i;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	Mtm = (MtmState*)ShmemInitStruct(MULTIMASTER_NAME, sizeof(MtmState) + sizeof(MtmNodeInfo)*(MtmMaxNodes-1), &found);
	if (!found)
	{
		MemSet(Mtm, 0, sizeof(MtmState) + sizeof(MtmNodeInfo)*(MtmMaxNodes-1));
		Mtm->status = MTM_DISABLED; //MTM_INITIALIZATION;
		Mtm->recoverySlot = 0;
		Mtm->locks = GetNamedLWLockTranche(MULTIMASTER_NAME);
		Mtm->csn = MtmGetCurrentTime();
		Mtm->lastCsn = INVALID_CSN;
		Mtm->oldestXid = FirstNormalTransactionId;
		Mtm->nLiveNodes = 0; //MtmNodes;
		Mtm->nAllNodes = MtmNodes;
		Mtm->disabledNodeMask =  (((nodemask_t)1 << MtmNodes) - 1);
		Mtm->clique = 0;
		Mtm->refereeGrant = false;
		Mtm->refereeWinnerId = 0;
		Mtm->stalledNodeMask = 0;
		Mtm->stoppedNodeMask = 0;
		Mtm->deadNodeMask = 0;
		Mtm->recoveredNodeMask = 0;
		Mtm->pglogicalReceiverMask = 0;
		Mtm->pglogicalSenderMask = 0;
		Mtm->inducedLockNodeMask = 0;
		Mtm->currentLockNodeMask = 0;
		Mtm->originLockNodeMask = 0;
		Mtm->reconnectMask = 0;
		Mtm->recoveredLSN = INVALID_LSN;
		Mtm->nActiveTransactions = 0;
		Mtm->nRunningTransactions = 0;
		Mtm->votingTransactions = NULL;
		Mtm->transListHead = NULL;
		Mtm->transListTail = &Mtm->transListHead;
		Mtm->activeTransList.next = Mtm->activeTransList.prev = &Mtm->activeTransList;
		Mtm->nReceivers = 0;
		Mtm->nSenders = 0;
		Mtm->timeShift = 0;
		Mtm->transCount = 0;
		Mtm->gcCount = 0;
		Mtm->nConfigChanges = 0;
		Mtm->recoveryCount = 0;
		Mtm->localTablesHashLoaded = false;
		Mtm->preparedTransactionsLoaded = false;
		Mtm->inject2PCError = 0;
		Mtm->sendQueue = NULL;
		Mtm->freeQueue = NULL;
		for (i = 0; i < MtmNodes; i++) {
			Mtm->nodes[i].oldestSnapshot = 0;
			Mtm->nodes[i].disabledNodeMask = 0;
			Mtm->nodes[i].connectivityMask = (((nodemask_t)1 << MtmNodes) - 1);
			Mtm->nodes[i].lockGraphUsed = 0;
			Mtm->nodes[i].lockGraphAllocated = 0;
			Mtm->nodes[i].lockGraphData = NULL;
			Mtm->nodes[i].transDelay = 0;
			Mtm->nodes[i].lastStatusChangeTime = MtmGetSystemTime();
			Mtm->nodes[i].con = MtmConnections[i];
			Mtm->nodes[i].flushPos = 0;
			Mtm->nodes[i].lastHeartbeat = 0;
			Mtm->nodes[i].restartLSN = INVALID_LSN;
			Mtm->nodes[i].originId = InvalidRepOriginId;
			Mtm->nodes[i].timeline = 0;
			Mtm->nodes[i].nHeartbeats = 0;
			Mtm->nodes[i].manualRecovery = false;
			Mtm->nodes[i].slotDeleted = false;
		}
		Mtm->nodes[MtmNodeId-1].originId = DoNotReplicateId;
		/* All transaction originated from the current node should be ignored during recovery */
		Mtm->nodes[MtmNodeId-1].restartLSN = (lsn_t)PG_UINT64_MAX;
		PGSemaphoreCreate(&Mtm->sendSemaphore);
		PGSemaphoreReset(&Mtm->sendSemaphore);
		SpinLockInit(&Mtm->queueSpinlock);
		BgwPoolInit(&Mtm->pool, MtmExecutor, MtmDatabaseName, MtmDatabaseUser, MtmQueueSize, MtmWorkers);
		RegisterXactCallback(MtmXactCallback, NULL);
		MtmTx.snapshot = INVALID_CSN;
		MtmTx.xid = InvalidTransactionId;
	}
	MtmXid2State = MtmCreateXidMap();
	MtmGid2State = MtmCreateGidMap();
	MtmLocalTables = MtmCreateLocalTableMap();
	MtmDoReplication = true;
	TM = &MtmTM;
	LWLockRelease(AddinShmemInitLock);

	MtmCheckControlFile();
}

static void
MtmShmemStartup(void)
{
	if (PreviousShmemStartupHook) {
		PreviousShmemStartupHook();
	}
	MtmInitialize();
}

static void MtmSetRemoteFunction(char const* list, void* extra)
{
	if (MtmRemoteFunctions) {
		hash_destroy(MtmRemoteFunctions);
		MtmRemoteFunctions = NULL;
	}
}

static void MtmInitializeRemoteFunctionsMap()
{
	HASHCTL info;
	char* p, *q;
	int n_funcs = 1;
	FuncCandidateList clist;

	for (p = MtmRemoteFunctionsList; (q = strchr(p, ',')) != NULL; p = q + 1, n_funcs++);

	Assert(MtmRemoteFunctions == NULL);

	memset(&info, 0, sizeof(info));
	info.entrysize = info.keysize = sizeof(Oid);
	info.hcxt = TopMemoryContext;
	MtmRemoteFunctions = hash_create("MtmRemoteFunctions", n_funcs, &info,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	p = pstrdup(MtmRemoteFunctionsList);
	do {
		q = strchr(p, ',');
		if (q != NULL) {
			*q++ = '\0';
		}
		clist = FuncnameGetCandidates(stringToQualifiedNameList(p), -1, NIL, false, false, true);
		if (clist == NULL) {
			MTM_ELOG(WARNING, "Failed to lookup function %s", p);
		} else if (clist->next != NULL) {
			MTM_ELOG(ERROR, "Ambigious function %s", p);
		} else {
			hash_search(MtmRemoteFunctions, &clist->oid, HASH_ENTER, NULL);
		}
		p = q;
	} while (p != NULL);

	clist = FuncnameGetCandidates(stringToQualifiedNameList("mtm.alter_sequences"), -1, NIL, false, false, true);
	if (clist != NULL) {
		hash_search(MtmRemoteFunctions, &clist->oid, HASH_ENTER, NULL);
	}
}

/*
 * Parse node connection string.
 * This function is called at cluster startup and while adding new cluster node
 */
void MtmUpdateNodeConnectionInfo(MtmConnectionInfo* conn, char const* connStr)
{
	char const* host;
	char const* end;
	int			hostLen;
	char const* port;
	int			connStrLen = (int)strlen(connStr);

	if (connStrLen >= MULTIMASTER_MAX_CONN_STR_SIZE) {
		MTM_ELOG(ERROR, "Too long (%d) connection string '%s': limit is %d",
			 connStrLen, connStr, MULTIMASTER_MAX_CONN_STR_SIZE-1);
	}

	while(isspace(*connStr))
		connStr++;

	strcpy(conn->connStr, connStr);

	host = strstr(connStr, "host=");
	if (host == NULL) {
		MTM_ELOG(ERROR, "Host not specified in connection string: '%s'", connStr);
	}
	host += 5;
	for (end = host; *end != ' ' && *end != '\0'; end++);
	hostLen = end - host;
	if (hostLen >= MULTIMASTER_MAX_HOST_NAME_SIZE) {
		MTM_ELOG(ERROR, "Too long (%d) host name '%.*s': limit is %d",
			 hostLen, hostLen, host, MULTIMASTER_MAX_HOST_NAME_SIZE-1);
	}
	memcpy(conn->hostName, host, hostLen);
	conn->hostName[hostLen] = '\0';

	port = strstr(connStr, "arbiter_port=");
	if (port != NULL) {
		if (sscanf(port+13, "%d", &conn->arbiterPort) != 1) {
			MTM_ELOG(ERROR, "Invalid arbiter port: %s", port+13);
		}
	} else {
		conn->arbiterPort = MULTIMASTER_DEFAULT_ARBITER_PORT;
	}
	MTM_ELOG(INFO, "Using arbiter port: %d", conn->arbiterPort);

	port = strstr(connStr, " port=");
	if (port == NULL && strncmp(connStr, "port=", 5) == 0) {
		port = connStr-1;
	}
	if (port != NULL) {
		if (sscanf(port+6, "%d", &conn->postmasterPort) != 1) {
			MTM_ELOG(ERROR, "Invalid postmaster port: %s", port+6);
		}
	} else {
		conn->postmasterPort = DEF_PGPORT;
	}
}

/*
 * Parse "multimaster.conn_strings" configuration parameter and
 * set connection string for each node using MtmUpdateNodeConnectionInfo
 */
static void MtmSplitConnStrs(void)
{
	int i;
	FILE* f = NULL;
	char buf[MULTIMASTER_MAX_CTL_STR_SIZE];
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	if (*MtmConnStrs == '@') {
		f = fopen(MtmConnStrs+1, "r");
		for (i = 0; fgets(buf, sizeof buf, f) != NULL; i++) {
			if (strlen(buf) <= 1) {
				MTM_ELOG(ERROR, "Empty lines are not allowed in %s file", MtmConnStrs+1);
			}
		}
	} else {
		char* p = MtmConnStrs;
		for (i = 0; *p != '\0'; p++, i++) {
			if ((p = strchr(p, ',')) == NULL) {
				i += 1;
				break;
			}
		}
	}

	if (i > MAX_NODES) {
		MTM_ELOG(ERROR, "Multimaster with more than %d nodes is not currently supported", MAX_NODES);
	}
	if (i < 2) {
		MTM_ELOG(ERROR, "Multimaster should have at least two nodes");
	}
	if (MtmMaxNodes == 0) {
		MtmMaxNodes = i;
	} else if (MtmMaxNodes < i) {
		MTM_ELOG(ERROR, "More than %d nodes are specified", MtmMaxNodes);
	}
	MtmNodes = i;
	MtmConnections = (MtmConnectionInfo*)palloc(MtmMaxNodes*sizeof(MtmConnectionInfo));

	if (f != NULL) {
		fseek(f, SEEK_SET, 0);
		for (i = 0; fgets(buf, sizeof buf, f) != NULL; i++) {
			size_t len = strlen(buf);
			if (buf[len-1] == '\n') {
				buf[len-1] = '\0';
			}
			MtmUpdateNodeConnectionInfo(&MtmConnections[i], buf);
		}
		fclose(f);
	} else {
		char* copy = pstrdup(MtmConnStrs);
		char* connStr = copy;
		char* connStrEnd = connStr + strlen(connStr);

		for (i = 0; connStr < connStrEnd; i++) {
			char* p = strchr(connStr, ',');
			if (p == NULL) {
				p = connStrEnd;
			}
			*p = '\0';
			MtmUpdateNodeConnectionInfo(&MtmConnections[i], connStr);
			connStr = p + 1;
		}
		pfree(copy);
	}

		if (MtmNodeId == INT_MAX) {
			if (gethostname(buf, sizeof buf) != 0) {
				MTM_ELOG(ERROR, "Failed to get host name: %m");
			}
			for (i = 0; i < MtmNodes; i++) {
				MTM_LOG3("Node %d, host %s, port=%d, my port %d", i, MtmConnections[i].hostName, MtmConnections[i].postmasterPort, PostPortNumber);
				if ((strcmp(MtmConnections[i].hostName, buf) == 0 || strcmp(MtmConnections[i].hostName, "localhost") == 0 || strcmp(MtmConnections[i].hostName, "127.0.0.1") == 0)
					&& MtmConnections[i].postmasterPort == PostPortNumber)
				{
					if (MtmNodeId == INT_MAX) {
						MtmNodeId = i+1;
					} else {
						MTM_ELOG(ERROR, "multimaster.node_id is not explicitly specified and more than one nodes are configured for host %s port %d", buf, PostPortNumber);
					}
				}
			}
			if (MtmNodeId == INT_MAX) {
				MTM_ELOG(ERROR, "multimaster.node_id is not specified and host name %s can not be located in connection strings list", buf);
			}
		} else if (MtmNodeId > i) {
			MTM_ELOG(ERROR, "Multimaster node id %d is out of range [%d..%d]", MtmNodeId, 1, MtmNodes);
		}
		{
			char* connStr = MtmConnections[MtmNodeId-1].connStr;
			char* dbName = strstr(connStr, "dbname="); // XXX: shoud we care about string 'itisnotdbname=xxx'?
			char* dbUser = strstr(connStr, "user=");
			char* end;
			size_t len;

			if (dbName == NULL)
				MTM_ELOG(ERROR, "Database is not specified in connection string: '%s'", connStr);

			if (dbUser == NULL)
			{
				char *errstr;
				const char *username = get_user_name(&errstr);
				if (!username)
					MTM_ELOG(FATAL, "Database user is not specified in connection string '%s', fallback failed: %s", connStr, errstr);
				else
					MTM_ELOG(WARNING, "Database user is not specified in connection string '%s', fallback to '%s'", connStr, username);
				MtmDatabaseUser = pstrdup(username);
			}
			else
			{
				dbUser += 5;
				end = strchr(dbUser, ' ');
				if (!end) end = strchr(dbUser, '\0');
				Assert(end != NULL);
				len = end - dbUser;
				MtmDatabaseUser = pnstrdup(dbUser, len);
			}

			dbName += 7;
			end = strchr(dbName, ' ');
			if (!end) end = strchr(dbName, '\0');
			Assert(end != NULL);
			len = end - dbName;
			MtmDatabaseName = pnstrdup(dbName, len);
		}
	MemoryContextSwitchTo(old_context);
}

/*
 * Check correctness of multimaster configuration
 */
static bool ConfigIsSane(void)
{
	bool ok = true;

#if 0
	if (DefaultXactIsoLevel != XACT_REPEATABLE_READ)
	{
		MTM_ELOG(WARNING, "multimaster requires default_transaction_isolation = 'repeatable read'");
		ok = false;
	}
#endif

	if (MtmMaxNodes < 1)
	{
		MTM_ELOG(WARNING, "multimaster requires multimaster.max_nodes > 0");
		ok = false;
	}

	if (max_prepared_xacts < 1)
	{
		MTM_ELOG(WARNING,
			 "multimaster requires max_prepared_transactions > 0, "
			 "because all transactions are implicitly two-phase");
		ok = false;
	}

	{
		int workers_required = 2 * MtmMaxNodes + MtmWorkers + 1;
		if (max_worker_processes < workers_required)
		{
			MTM_ELOG(WARNING,
				 "multimaster requires max_worker_processes >= %d",
				 workers_required);
			ok = false;
		}
	}

	if (wal_level != WAL_LEVEL_LOGICAL)
	{
		MTM_ELOG(WARNING,
			 "multimaster requires wal_level = 'logical', "
			 "because it is build on top of logical replication");
		ok = false;
	}

	if (max_wal_senders < MtmMaxNodes)
	{
		MTM_ELOG(WARNING,
			 "multimaster requires max_wal_senders >= %d (multimaster.max_nodes), ",
			 MtmMaxNodes);
		ok = false;
	}

	if (max_replication_slots < MtmMaxNodes)
	{
		MTM_ELOG(WARNING,
			 "multimaster requires max_replication_slots >= %d (multimaster.max_nodes), ",
			 MtmMaxNodes);
		ok = false;
	}

	return ok;
}

void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.	 (We don't throw error here because it seems useful to
	 * allow the cs_* functions to be created even when the
	 * module isn't active.	 The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable(
		"multimaster.heartbeat_send_timeout",
		"Timeout in milliseconds of sending heartbeat messages",
		"Period of broadcasting heartbeat messages by arbiter to all nodes",
		&MtmHeartbeatSendTimeout,
		200,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.heartbeat_recv_timeout",
		"Timeout in milliseconds of receiving heartbeat messages",
		"If no heartbeat message is received from node within this period, it assumed to be dead",
		&MtmHeartbeatRecvTimeout,
		1000,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.gc_period",
		"Number of distributed transactions after which garbage collection is started",
		"Multimaster is building xid->csn hash map which has to be cleaned to avoid hash overflow. This parameter specifies interval of invoking garbage collector for this map",
		&MtmGcPeriod,
		1000,
		1,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_nodes",
		"Maximal number of cluster nodes",
		"This parameters allows to add new nodes to the cluster, default value 0 restricts number of nodes to one specified in multimaster.conn_strings",
		&MtmMaxNodes,
		0,
		0,
		MAX_NODES,
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomIntVariable(
		"multimaster.trans_spill_threshold",
		"Maximal size of transaction after which transaction is written to the disk",
		NULL,
		&MtmTransSpillThreshold,
		100 * 1024, /* 100Mb */
		0,
		MaxAllocSize/1024,
		PGC_SIGHUP,
		GUC_UNIT_KB,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.node_disable_delay",
		"Minimal amount of time (msec) between node status change",
		"This delay is used to avoid false detection of node failure and to prevent blinking of node status node",
		&MtmNodeDisableDelay,
		2000,
		1,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.min_recovery_lag",
		"Minimal lag of WAL-sender performing recovery after which cluster is locked until recovery is completed",
		"When wal-sender almost catch-up WAL current position we need to stop 'Achilles tortile competition' and "
		"temporary stop commit of new transactions until node will be completely repared",
		&MtmMinRecoveryLag,
		10, /* 10 kB */
		0,
		INT_MAX,
		PGC_SIGHUP,
		GUC_UNIT_KB,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_recovery_lag",
		"Maximal lag of replication slot of failed node after which this slot is dropped to avoid transaction log overflow",
		"Dropping slot makes it not possible to recover node using logical replication mechanism, it will be ncessary to completely copy content of some other nodes "
		"using basebackup or similar tool. Zero value of parameter disable dropping slot.",
		&MtmMaxRecoveryLag,
		1 * 1024 * 1024, /* 1 GB */
		0,
		INT_MAX,
		PGC_SIGHUP,
		GUC_UNIT_KB,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.break_connection",
		"Break connection with client when node is no online",
		NULL,
		&MtmBreakConnection,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.bypass",
		"Allow access to offline multimaster node",
		NULL,
		&MtmBypass,
		false,
		PGC_USERSET, /* context */
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.major_node",
		"Node which forms a majority in case of partitioning in cliques with equal number of nodes",
		NULL,
		&MtmMajorNode,
		false,
		PGC_SUSET,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.monotonic_sequences",
		"Enforce monotinic behaviour of sequence values obtained from different nodes",
		NULL,
		&MtmMonotonicSequences,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.ignore_tables_without_pk",
		"Do not replicate tables without primary key",
		NULL,
		&MtmIgnoreTablesWithoutPk,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.use_dtm",
		"Use distributed transaction manager",
		NULL,
		&MtmUseDtm,
		true,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.referee",
		"This instance of Postgres contains no data and peforms role of referee for other nodes",
		NULL,
		&MtmReferee,
		false,
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"multimaster.referee_connstring",
		"Referee connection string",
		NULL,
		&MtmRefereeConnStr,
		"",
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.use_rdma",
		"Use RDMA sockets",
		NULL,
		&MtmUseRDMA,
		false,
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.preserve_commit_order",
		"Transactions from one node will be committed in same order on all nodes",
		NULL,
		&MtmPreserveCommitOrder,
		true,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.volkswagen_mode",
		"Pretend to be normal postgres. This means skip some NOTICE's and use local sequences. Default false.",
		NULL,
		&MtmVolksWagenMode,
		false,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.workers",
		"Number of multimaster executor workers",
		NULL,
		&MtmWorkers,
		8,
		1,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_workers",
		"Maximal number of multimaster dynamic executor workers",
		NULL,
		&MtmMaxWorkers,
		100,
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.vacuum_delay",
		"Minimal age of records which can be vacuumed (seconds)",
		NULL,
		&MtmVacuumDelay,
		4,
		1,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.min_2pc_timeout",
		"Minimal timeout between receiving PREPARED message from nodes participated in transaction to coordinator (milliseconds)",
		NULL,
		&MtmMin2PCTimeout,
		0, /* disabled */
		0,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_2pc_ratio",
		"Maximal ratio (in percents) between prepare time at different nodes: if T is time of preparing transaction at some node,"
			" then transaction can be aborted if prepared response was not received in T*MtmMax2PCRatio/100",
		NULL,
		&MtmMax2PCRatio,
		200, /* 2 times */
		1,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.queue_size",
		"Multimaster queue size",
		NULL,
		&MtmQueueSize,
		256*1024*1024,
		1024*1024,
		INT_MAX,
		PGC_BACKEND,
		GUC_NO_SHOW_ALL,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.arbiter_port",
		"Base value for assigning arbiter ports",
		NULL,
		&MtmArbiterPort,
		MULTIMASTER_DEFAULT_ARBITER_PORT,
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"multimaster.conn_strings",
		"Multimaster node connection strings separated by commas, i.e. 'replication=database dbname=postgres host=localhost port=5001,replication=database dbname=postgres host=localhost port=5002'",
		NULL,
		&MtmConnStrs,
		"",
		PGC_BACKEND, /* context */
		0,			 /* flags */
		NULL,		 /* GucStringCheckHook check_hook */
		NULL,		 /* GucStringAssignHook assign_hook */
		NULL		 /* GucShowHook show_hook */
	);

	DefineCustomStringVariable(
		"multimaster.remote_functions",
		"List of function names which should be executed remotely at all multimaster nodes instead of executing them at master and replicating result of their work",
		NULL,
		&MtmRemoteFunctionsList,
		"lo_create,lo_unlink",
		PGC_USERSET, /* context */
		GUC_LIST_INPUT, /* flags */
		NULL,		 /* GucStringCheckHook check_hook */
		MtmSetRemoteFunction,		 /* GucStringAssignHook assign_hook */
		NULL		 /* GucShowHook show_hook */
	);

	DefineCustomStringVariable(
		"multimaster.cluster_name",
		"Name of the cluster",
		NULL,
		&MtmClusterName,
		"mmts",
		PGC_BACKEND, /* context */
		0,			 /* flags */
		NULL,		 /* GucStringCheckHook check_hook */
		NULL,		 /* GucStringAssignHook assign_hook */
		NULL		 /* GucShowHook show_hook */
	);

	DefineCustomIntVariable(
		"multimaster.node_id",
		"Multimaster node ID",
		NULL,
		&MtmNodeId,
		INT_MAX,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	if (MtmReferee)
	{
		return;
	}

	/* This will also perform some checks on connection strings */
	MtmSplitConnStrs();

	if (!ConfigIsSane()) {
		MTM_ELOG(ERROR, "Multimaster config is insane, refusing to work");
	}

	MtmStartReceivers();

	/*
	 * Request additional shared resources.	 (These are no-ops if we're not in
	 * the postmaster process.)	 We'll allocate or attach to the shared
	 * resources in mtm_shmem_startup().
	 */
	RequestAddinShmemSpace(MTM_SHMEM_SIZE + MtmQueueSize);
	RequestNamedLWLockTranche(MULTIMASTER_NAME, 1 + MtmMaxNodes*2);

	BgwPoolStart(MtmWorkers, MtmPoolConstructor);

	MtmArbiterInitialize();

	/*
	 * Install hooks.
	 */
	PreviousShmemStartupHook = shmem_startup_hook;
	shmem_startup_hook = MtmShmemStartup;

	PreviousExecutorStartHook = ExecutorStart_hook;
	ExecutorStart_hook = MtmExecutorStart;

	PreviousExecutorFinishHook = ExecutorFinish_hook;
	ExecutorFinish_hook = MtmExecutorFinish;

	PreviousProcessUtilityHook = ProcessUtility_hook;
	ProcessUtility_hook = MtmProcessUtility;

	PreviousSeqNextvalHook = SeqNextvalHook;
	SeqNextvalHook = MtmSeqNextvalHook;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	shmem_startup_hook = PreviousShmemStartupHook;
	ExecutorFinish_hook = PreviousExecutorFinishHook;
	ProcessUtility_hook = PreviousProcessUtilityHook;
	SeqNextvalHook = PreviousSeqNextvalHook;
}


/*
 * Recovery slot is node ID from which new or crash node is performing recovery.
 * This function is called in case of logical receiver error to make it possible to try to perform
 * recovery from some other node
 */
void MtmReleaseRecoverySlot(int nodeId)
{
	if (Mtm->recoverySlot == nodeId) {
		Mtm->recoverySlot = 0;
	}
}

/*
 * Rollback transaction originated from the specified node.
 * This function is called either for commit logical message with AbortPrepared flag either for abort prepared logical message.
 */
void MtmRollbackPreparedTransaction(int nodeId, char const* gid)
{
	char state3pc[MAX_3PC_STATE_SIZE];
	XidStatus status = MtmExchangeGlobalTransactionStatus(gid, TRANSACTION_STATUS_ABORTED);
	MTM_LOG1("Abort prepared transaction %s status %s from node %d originId=%d", gid, MtmTxnStatusMnem[status], nodeId, Mtm->nodes[nodeId-1].originId);
	if (status == TRANSACTION_STATUS_UNKNOWN || (status == TRANSACTION_STATUS_IN_PROGRESS && GetPreparedTransactionState(gid, state3pc)))
	{
		MTM_LOG1("PGLOGICAL_ABORT_PREPARED commit: gid=%s #2", gid);
		MtmResetTransaction();
		StartTransactionCommand();
		MtmBeginSession(nodeId);
		MtmSetCurrentTransactionGID(gid);
		TXFINISH("%s ABORT, MtmRollbackPrepared", gid);
		FinishPreparedTransaction(gid, false);
		MtmTx.isActive = true;
		CommitTransactionCommand();
		Assert(!MtmTx.isActive);
		MtmEndSession(nodeId, true);
	} else if (status == TRANSACTION_STATUS_IN_PROGRESS) {
		MtmBeginSession(nodeId);
		MtmLogAbortLogicalMessage(nodeId, gid);
		MtmEndSession(nodeId, true);
	}
}

/*
 * Wrapper around FinishPreparedTransaction function.
 * A proper context is required for invocation of this function.
 * This function is called with MTM mutex locked.
 * It should unlock mutex before calling FinishPreparedTransaction to avoid deadlocks.
 * `ts` object is pinned to prevent deallocation while lock is released.
 */
void MtmFinishPreparedTransaction(MtmTransState* ts, bool commit)
{
	bool insideTransaction = IsTransactionState();

	Assert(ts->votingCompleted);

	ts->isPinned = true;
	MtmUnlock();

	MtmResetTransaction();

	if (!insideTransaction) {
		StartTransactionCommand();
	}
	MtmSetCurrentTransactionCSN(ts->csn);
	MtmSetCurrentTransactionGID(ts->gid);
	MtmTx.isActive = true;
	FinishPreparedTransaction(ts->gid, commit);
	if (commit) {
		MTM_LOG2("Distributed transaction %s (%lld) is committed at %lld with LSN=%lld", ts->gid, (long64)ts->xid, MtmGetCurrentTime(), (long64)GetXLogInsertRecPtr());
	}
	if (!insideTransaction) {
		CommitTransactionCommand();
		Assert(!MtmTx.isActive);
		Assert(ts->status == commit ? TRANSACTION_STATUS_COMMITTED : TRANSACTION_STATUS_ABORTED);
	}

	MtmLock(LW_EXCLUSIVE);
	ts->isPinned = false;
}

/*
 * Determine when and how we should open replication slot.
 * During recovery we need to open only one replication slot from which node should receive all transactions.
 * Slots at other nodes should be removed.
 */
MtmReplicationMode MtmGetReplicationMode(int nodeId, sig_atomic_t volatile* shutdown)
{
	MtmLock(LW_EXCLUSIVE);

	if (!Mtm->preparedTransactionsLoaded)
	{
		/* We must restore state of prepared (but no committed or aborted) transaction before start of recovery. */
		MtmLoadPreparedTransactions();
		Mtm->preparedTransactionsLoaded = true;
	}

	/* Await until node is connected and both receiver and sender are in clique */
	while (BIT_CHECK(EFFECTIVE_CONNECTIVITY_MASK, nodeId - 1) ||
			BIT_CHECK(EFFECTIVE_CONNECTIVITY_MASK, MtmNodeId - 1))
	{
		MtmUnlock();
		if (*shutdown)
			return REPLMODE_EXIT;
		MtmSleep(STATUS_POLL_DELAY);
		MtmLock(LW_EXCLUSIVE);
	}

	if (BIT_CHECK(Mtm->disabledNodeMask, MtmNodeId - 1))
	{
		/* Ok, then start recovery by luckiest walreceiver (if there is no donor node).
		 * If this node was populated using basebackup, then donorNodeId is not zero and we should choose this node for recovery */
		if ((Mtm->recoverySlot == 0 || Mtm->recoverySlot == nodeId)
			&& (Mtm->donorNodeId == MtmNodeId || Mtm->donorNodeId == nodeId))
		{
			/* Lock on us */
			Mtm->recoverySlot = nodeId;
			MtmPollStatusOfPreparedTransactions(false);
			MtmUnlock();
			return REPLMODE_RECOVERY;
		}

		/* And force less lucky walreceivers wait until recovery is completed */
		while (BIT_CHECK(Mtm->disabledNodeMask, MtmNodeId - 1))
		{
			MtmUnlock();
			if (*shutdown)
				return REPLMODE_EXIT;
			MtmSleep(STATUS_POLL_DELAY);
			MtmLock(LW_EXCLUSIVE);
		}
	}

	MtmUnlock();
	return REPLMODE_RECOVERED;
}

static bool MtmIsBroadcast()
{
	return application_name != NULL && strcmp(application_name, MULTIMASTER_BROADCAST_SERVICE) == 0;
}

/*
 * Recover node is needed to return stopped and newly added node to the cluster.
 * This function creates logical replication slot for the node which will collect
 * all changes which should be sent to this node from this moment.
 */
void MtmRecoverNode(int nodeId)
{
	if (nodeId <= 0 || nodeId > Mtm->nAllNodes)
	{
		MTM_ELOG(ERROR, "NodeID %d is out of range [1,%d]", nodeId, Mtm->nAllNodes);
	}
	MtmLock(LW_EXCLUSIVE);
	Mtm->nodes[nodeId-1].manualRecovery = true;
	if (BIT_CHECK(Mtm->stoppedNodeMask, nodeId-1))
	{
		Assert(BIT_CHECK(Mtm->disabledNodeMask, nodeId-1));
		BIT_CLEAR(Mtm->stoppedNodeMask, nodeId-1);
		BIT_CLEAR(Mtm->stalledNodeMask, nodeId-1);
	}
	MtmUnlock();

	if (!MtmIsBroadcast())
	{
		MtmBroadcastUtilityStmt(psprintf("select pg_create_logical_replication_slot('" MULTIMASTER_SLOT_PATTERN "', '" MULTIMASTER_NAME "')", nodeId), true, 0);
		MtmBroadcastUtilityStmt(psprintf("select mtm.recover_node(%d)", nodeId), true, 0);
	}
}

/*
 * Resume previosly stopped node.
 * This function creates logical replication slot for the node which will collect
 * all changes which should be sent to this node from this moment.
 */
void MtmResumeNode(int nodeId)
{
	if (nodeId <= 0 || nodeId > Mtm->nAllNodes)
	{
		MTM_ELOG(ERROR, "NodeID %d is out of range [1,%d]", nodeId, Mtm->nAllNodes);
	}
	MtmLock(LW_EXCLUSIVE);
	if (BIT_CHECK(Mtm->stalledNodeMask, nodeId-1))
	{
		MtmUnlock();
		MTM_ELOG(ERROR, "Node %d can not be resumed because it's replication slot is dropped", nodeId);
	}
	if (BIT_CHECK(Mtm->stoppedNodeMask, nodeId-1))
	{
		Assert(BIT_CHECK(Mtm->disabledNodeMask, nodeId-1));
		BIT_CLEAR(Mtm->stoppedNodeMask, nodeId-1);
	}
	MtmUnlock();

	if (!MtmIsBroadcast())
	{
		MtmBroadcastUtilityStmt(psprintf("select mtm.resume_node(%d)", nodeId), true, nodeId);
	}
}

/*
 * Permanently exclude node from the cluster. Node will not participate in voting and can not be automatically recovered
 * until MtmRecoverNode is invoked.
 */
void MtmStopNode(int nodeId, bool dropSlot)
{
	if (nodeId <= 0 || nodeId > Mtm->nAllNodes)
	{
		MTM_ELOG(ERROR, "NodeID %d is out of range [1,%d]", nodeId, Mtm->nAllNodes);
	}

	if (!MtmIsBroadcast())
	{
		MtmBroadcastUtilityStmt(psprintf("select mtm.stop_node(%d,%s)", nodeId, dropSlot ? "true" : "false"), true, nodeId);
	}

	MtmLock(LW_EXCLUSIVE);
	BIT_SET(Mtm->stoppedNodeMask, nodeId-1);
	if (!BIT_CHECK(Mtm->disabledNodeMask, nodeId-1))
	{
		MtmDisableNode(nodeId);
	}
	MtmUnlock();

	if (dropSlot)
	{
		MtmDropSlot(nodeId);
	}
}

static void
MtmOnProcExit(int code, Datum arg)
{
	if (MtmReplicationNodeId > 0) {
		Mtm->nodes[MtmReplicationNodeId-1].senderPid = -1;
		MTM_LOG1("WAL-sender to %d is terminated", MtmReplicationNodeId);
		/* MtmOnNodeDisconnect(MtmReplicationNodeId); */
	}
}

static void
MtmReplicationStartupHook(struct PGLogicalStartupHookArgs* args)
{
	ListCell *param;
	bool recoveryCompleted = false;
	ulong64 recoveryStartPos = INVALID_LSN;

	MtmIsRecoverySession = false;
	Mtm->nodes[MtmReplicationNodeId-1].senderPid = MyProcPid;
	Mtm->nodes[MtmReplicationNodeId-1].senderStartTime = MtmGetSystemTime();
	foreach(param, args->in_params)
	{
		DefElem	   *elem = lfirst(param);
		if (strcmp("mtm_replication_mode", elem->defname) == 0) {
			if (elem->arg != NULL && strVal(elem->arg) != NULL) {
				if (strcmp(strVal(elem->arg), "recovery") == 0) {
					MtmIsRecoverySession = true;
				} else if (strcmp(strVal(elem->arg), "recovered") == 0) {
					recoveryCompleted = true;
				} else if (strcmp(strVal(elem->arg), "open_existed") != 0 && strcmp(strVal(elem->arg), "create_new") != 0) {
					MTM_ELOG(ERROR, "Illegal recovery mode %s", strVal(elem->arg));
				}
			} else {
				MTM_ELOG(ERROR, "Replication mode is not specified");
			}
		} else if (strcmp("mtm_restart_pos", elem->defname) == 0) {
			if (elem->arg != NULL && strVal(elem->arg) != NULL) {
				sscanf(strVal(elem->arg), "%llx", &recoveryStartPos);
			} else {
				MTM_ELOG(ERROR, "Restart position is not specified");
			}
		} else if (strcmp("mtm_recovered_pos", elem->defname) == 0) {
			if (elem->arg != NULL && strVal(elem->arg) != NULL) {
				ulong64 recoveredLSN;
				sscanf(strVal(elem->arg), "%llx", &recoveredLSN);
				MTM_LOG1("Recovered position of node %d is %llx", MtmReplicationNodeId, recoveredLSN);
				if (Mtm->nodes[MtmReplicationNodeId-1].restartLSN < recoveredLSN) {
					MTM_LOG1("Advance restartLSN for node %d from %llx to %llx (MtmReplicationStartupHook)",
							 MtmReplicationNodeId, Mtm->nodes[MtmReplicationNodeId-1].restartLSN, recoveredLSN);
					// Assert(Mtm->nodes[MtmReplicationNodeId-1].restartLSN == INVALID_LSN
					// 	   || recoveredLSN < Mtm->nodes[MtmReplicationNodeId-1].restartLSN + MtmMaxRecoveryLag);
					Mtm->nodes[MtmReplicationNodeId-1].restartLSN = recoveredLSN;
				}
			} else {
				MTM_ELOG(ERROR, "Recovered position is not specified");
			}
		}
	}
	MTM_LOG1("Startup of logical replication to node %d", MtmReplicationNodeId);
	MtmLock(LW_EXCLUSIVE);

	if (BIT_CHECK(Mtm->stalledNodeMask, MtmReplicationNodeId-1)) {
		MtmUnlock();
		MTM_ELOG(ERROR, "Stalled node %d tries to initiate recovery", MtmReplicationNodeId);
	}

	if (BIT_CHECK(Mtm->stoppedNodeMask, MtmReplicationNodeId-1)) {
		MtmUnlock();
		MTM_ELOG(ERROR, "Stopped node %d tries to connect", MtmReplicationNodeId);
	}

	if (MtmIsRecoverySession) {
		MTM_LOG1("%d: Node %d start recovery of node %d at position %llx", MyProcPid, MtmNodeId, MtmReplicationNodeId, recoveryStartPos);
		Assert(MyReplicationSlot != NULL);
		if (recoveryStartPos < MyReplicationSlot->data.restart_lsn) {
			MTM_ELOG(WARNING, "Specified recovery start position %llx is beyond restart lsn %llx", recoveryStartPos, (long64)MyReplicationSlot->data.restart_lsn);
		}
		MtmStateProcessNeighborEvent(MtmReplicationNodeId, MTM_NEIGHBOR_WAL_SENDER_START_RECOVERY);
	} else { //if (BIT_CHECK(Mtm->disabledNodeMask,	 MtmReplicationNodeId-1)) {
		if (recoveryCompleted) {
			MTM_LOG1("Node %d consider that recovery of node %d is completed: start normal replication", MtmNodeId, MtmReplicationNodeId);
			MtmStateProcessNeighborEvent(MtmReplicationNodeId, MTM_NEIGHBOR_WAL_SENDER_START_RECOVERED);
		} else {
			/* Force arbiter to reestablish connection with this node, send heartbeat to inform this node that it was disabled and should perform recovery */
			BIT_SET(Mtm->reconnectMask, MtmReplicationNodeId-1);
			MtmUnlock();
			MTM_ELOG(ERROR, "Disabled node %d tries to reconnect without recovery", MtmReplicationNodeId);
		}
	}
	// else {
	// 	// MTM_LOG1("Node %d start logical replication to node %d in normal mode", MtmNodeId, MtmReplicationNodeId);
	// 	MtmStateProcessNeighborEvent(MtmReplicationNodeId, MTM_NEIGHBOR_WAL_SENDER_START_NORMAL);
	// }

	BIT_SET(Mtm->reconnectMask, MtmReplicationNodeId-1); /* arbiter should try to reestablish connection with this node */
	MtmUnlock();
	on_shmem_exit(MtmOnProcExit, 0);
}

lsn_t MtmGetFlushPosition(int nodeId)
{
	return Mtm->nodes[nodeId-1].flushPos;
}

/**
 * Keep track of progress of WAL writer.
 * We need to notify WAL senders at other nodes which logical records
 * are flushed to the disk and so can survive failure. In asynchronous commit mode
 * WAL is flushed by WAL writer. Current flush position can be obtained by GetFlushRecPtr().
 * So on applying new logical record we insert it in the MtmLsnMapping and compare
 * their poistions in local WAL log with current flush position.
 * The records which are flushed to the disk by WAL writer are removed from the list
 * and mapping ing mtm->nodes[].flushPos is updated for this node.
 */
void  MtmUpdateLsnMapping(int node_id, lsn_t end_lsn)
{
	dlist_mutable_iter iter;
	MtmFlushPosition* flushpos;
	lsn_t local_flush = GetFlushRecPtr();
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	if (end_lsn != INVALID_LSN) {
		/* Track commit lsn */
		flushpos = (MtmFlushPosition *) palloc(sizeof(MtmFlushPosition));
		flushpos->node_id = node_id;
		flushpos->local_end = XactLastCommitEnd;
		flushpos->remote_end = end_lsn;
		dlist_push_tail(&MtmLsnMapping, &flushpos->node);
	}
	MtmLock(LW_EXCLUSIVE);
	dlist_foreach_modify(iter, &MtmLsnMapping)
	{
		flushpos = dlist_container(MtmFlushPosition, node, iter.cur);
		if (flushpos->local_end <= local_flush)
		{
			if (Mtm->nodes[node_id-1].flushPos < flushpos->remote_end) {
				Mtm->nodes[node_id-1].flushPos = flushpos->remote_end;
			}
			dlist_delete(iter.cur);
			pfree(flushpos);
		} else {
			break;
		}
	}
	MtmUnlock();
	MemoryContextSwitchTo(old_context);
}


static void
MtmReplicationShutdownHook(struct PGLogicalShutdownHookArgs* args)
{
	MtmLock(LW_EXCLUSIVE);
	if (MtmReplicationNodeId >= 0 && BIT_CHECK(Mtm->pglogicalSenderMask, MtmReplicationNodeId-1)) {
		BIT_CLEAR(Mtm->pglogicalSenderMask, MtmReplicationNodeId-1);
		Mtm->nSenders -= 1;
		MTM_LOG1("Logical replication to node %d is stopped", MtmReplicationNodeId);
		/* MtmOnNodeDisconnect(MtmReplicationNodeId); */
		MtmReplicationNodeId = -1; /* defuse MtmOnProcExit hook */
	}
	MtmUnlock();
}

/*
 * Filter transactions which should be replicated to other nodes.
 * This filter is applied at sender side (WAL sender).
 * Final filtering is also done at destination side by MtmFilterTransaction function.
 */
static bool
MtmReplicationTxnFilterHook(struct PGLogicalTxnFilterArgs* args)
{
	/* Do not replicate any transactions in recovery mode (because we should apply
	 * changes sent to us rather than send our own pending changes)
	 * and transactions received from other nodes
	 * (originId should be non-zero in this case)
	 * unless we are performing recovery of disabled node
	 * (in this case all transactions should be sent)
	 */
	/*
	 * I removed (Mtm->status != MTM_RECOVERY) here since in major
	 * mode we need to recover from offline node too. Also it seems
	 * that with amount of nodes >= 3 we also need that. --sk
	 *
	 * On a first look this works fine.
	 */
	bool res = (args->origin_id == InvalidRepOriginId
			|| MtmIsRecoveredNode(MtmReplicationNodeId));
	if (!res) {
		MTM_LOG2("Filter transaction with origin_id=%d", args->origin_id);
	}
	return res;
}

/**
 * Filter record corresponding to local (non-distributed) tables
 */
static bool
MtmReplicationRowFilterHook(struct PGLogicalRowFilterArgs* args)
{
	bool isDistributed;
	MtmLock(LW_SHARED);
	if (!Mtm->localTablesHashLoaded) {
		MtmUnlock();
		MtmLock(LW_EXCLUSIVE);
		if (!Mtm->localTablesHashLoaded) {
			MtmLoadLocalTables();
			Mtm->localTablesHashLoaded = true;
		}
	}
	isDistributed = hash_search(MtmLocalTables, &RelationGetRelid(args->changed_rel), HASH_FIND, NULL) == NULL;
	MtmUnlock();
	return isDistributed;
}

/*
 * Filter received transactions at destination side.
 * This function is executed by receiver,
 * so there are no race conditions and it is possible to update nodes[i].restartLSN without lock.
 * It is more efficient to filter records at senders size (done by MtmReplicationTxnFilterHook) to avoid sending useless data through network.
 * But asynchronous nature of logical replications makes it not possible to guarantee (at least I failed to do it)
 * that replica do not receive deteriorated data.
 */
bool MtmFilterTransaction(char* record, int size)
{
	StringInfoData s;
	uint8		event;
	lsn_t		origin_lsn;
	lsn_t		end_lsn;
	lsn_t		restart_lsn;
	int			replication_node;
	int			origin_node;
	char const* gid = "";
	char		msgtype PG_USED_FOR_ASSERTS_ONLY;
	bool		duplicate = false;

	s.data = record;
	s.len = size;
	s.maxlen = -1;
	s.cursor = 0;

	msgtype = pq_getmsgbyte(&s);
	Assert(msgtype == 'C');
	event = pq_getmsgbyte(&s); /* event */
	replication_node = pq_getmsgbyte(&s);

	/* read fields */
	pq_getmsgint64(&s); /* commit_lsn */
	end_lsn = pq_getmsgint64(&s); /* end_lsn */
	pq_getmsgint64(&s); /* commit_time */

	origin_node = pq_getmsgbyte(&s);
	origin_lsn = pq_getmsgint64(&s);

	Assert(replication_node == MtmReplicationNodeId);
	if (!(origin_node != 0 &&
		  (Mtm->status == MTM_RECOVERY || origin_node == replication_node)))
	{
		MTM_ELOG(WARNING, "Receive redirected commit event %d from node %d origin node %d origin LSN %llx in %s mode",
			 event, replication_node, origin_node, origin_lsn, MtmNodeStatusMnem[Mtm->status]);
	}

	switch (event)
	{
	  case PGLOGICAL_PREPARE:
	  case PGLOGICAL_PRECOMMIT_PREPARED:
	  case PGLOGICAL_ABORT_PREPARED:
		gid = pq_getmsgstring(&s);
		break;
	  case PGLOGICAL_COMMIT_PREPARED:
		pq_getmsgint64(&s); /* CSN */
		gid = pq_getmsgstring(&s);
		break;
	  default:
		break;
	}
	restart_lsn = origin_node == MtmReplicationNodeId ? end_lsn : origin_lsn;
	if (Mtm->nodes[origin_node-1].restartLSN < restart_lsn) {
		MTM_LOG2("[restartlsn] node %d: %llx -> %llx (MtmFilterTransaction)", MtmReplicationNodeId, Mtm->nodes[MtmReplicationNodeId-1].restartLSN, restart_lsn);
		// if (event != PGLOGICAL_PREPARE) {
		// 	/* Transactions can be prepared in different order, so to avoid loosing transactions we should not update restartLsn for them */
		// 	Mtm->nodes[origin_node-1].restartLSN = restart_lsn;
		// }
	} else {
		duplicate = true;
	}

	if (duplicate) {
		MTM_LOG1("Ignore transaction %s from node %d event=%x because our LSN position %llx for origin node %d is greater or equal than LSN %llx of this transaction (end_lsn=%llx, origin_lsn=%llx) mode %s",
				 gid, replication_node, event, Mtm->nodes[origin_node-1].restartLSN, origin_node, restart_lsn, end_lsn, origin_lsn, MtmNodeStatusMnem[Mtm->status]);
	} else {
		MTM_LOG2("Apply transaction %s from node %d lsn %llx, event=%x, origin node %d, original lsn=%llx, current lsn=%llx",
				 gid, replication_node, end_lsn, event, origin_node, origin_lsn, restart_lsn);
	}

	return duplicate;
}

void MtmSetupReplicationHooks(struct PGLogicalHooks* hooks)
{
	hooks->startup_hook = MtmReplicationStartupHook;
	hooks->shutdown_hook = MtmReplicationShutdownHook;
	hooks->txn_filter_hook = MtmReplicationTxnFilterHook;
	hooks->row_filter_hook = MtmReplicationRowFilterHook;
}

/*
 * Setup replication session origin to include origin location in WAL and
 * update slot position.
 * Sessions are not reetrant so we have to use exclusive lock here.
 */
void MtmBeginSession(int nodeId)
{
	// MtmLockNode(nodeId, LW_EXCLUSIVE);
	Assert(replorigin_session_origin == InvalidRepOriginId);
	replorigin_session_origin = Mtm->nodes[nodeId-1].originId;
	Assert(replorigin_session_origin != InvalidRepOriginId);
	MTM_LOG3("%d: Begin setup replorigin session: %d", MyProcPid, replorigin_session_origin);
	replorigin_session_setup(replorigin_session_origin);
	MTM_LOG3("%d: End setup replorigin session: %d", MyProcPid, replorigin_session_origin);
}

/*
 * Release replication session
 */
void MtmEndSession(int nodeId, bool unlock)
{
	if (replorigin_session_origin != InvalidRepOriginId) {
		MTM_LOG2("%d: Begin reset replorigin session for node %d: %d, progress %llx", MyProcPid, nodeId, replorigin_session_origin, replorigin_session_get_progress(false));
		replorigin_session_origin = InvalidRepOriginId;
		replorigin_session_origin_lsn = INVALID_LSN;
		replorigin_session_origin_timestamp = 0;
		replorigin_session_reset();
		// if (unlock) {
		// 	MtmUnlockNode(nodeId);
		// }
		MTM_LOG3("%d: End reset replorigin session: %d", MyProcPid, replorigin_session_origin);
	}
}


/*
 * -------------------------------------------
 * SQL API functions
 * -------------------------------------------
 */


Datum
mtm_start_replication(PG_FUNCTION_ARGS)
{
	MtmDoReplication = true;
	PG_RETURN_VOID();
}

Datum
mtm_stop_replication(PG_FUNCTION_ARGS)
{
	MtmDoReplication = false;
	MtmTx.isDistributed = false;
	PG_RETURN_VOID();
}

Datum
mtm_stop_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	bool dropSlot = PG_GETARG_BOOL(1);
	MtmStopNode(nodeId, dropSlot);
	PG_RETURN_VOID();
}

Datum
mtm_add_node(PG_FUNCTION_ARGS)
{
	char *connStr = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (Mtm->nAllNodes == MtmMaxNodes) {
		MTM_ELOG(ERROR, "Maximal number of nodes %d is reached", MtmMaxNodes);
	}
	if (!MtmIsBroadcast())
	{
		MtmBroadcastUtilityStmt(psprintf("select pg_create_logical_replication_slot('" MULTIMASTER_SLOT_PATTERN "', '" MULTIMASTER_NAME "')", Mtm->nAllNodes+1), true, 0);
		MtmBroadcastUtilityStmt(psprintf("select mtm.add_node('%s')", connStr), true, 0);
	}
	else
	{
		int nodeId;
		MtmLock(LW_EXCLUSIVE);
		nodeId = Mtm->nAllNodes;
		MTM_ELOG(NOTICE, "Add node %d: '%s'", nodeId+1, connStr);

		MtmUpdateNodeConnectionInfo(&Mtm->nodes[nodeId].con, connStr);

		if (*MtmConnStrs == '@') {
			FILE* f = fopen(MtmConnStrs+1, "a");
			fprintf(f, "%s\n", connStr);
			fclose(f);
		}

		Mtm->nodes[nodeId].transDelay = 0;
		Mtm->nodes[nodeId].lastStatusChangeTime = MtmGetSystemTime();
		Mtm->nodes[nodeId].flushPos = 0;
		Mtm->nodes[nodeId].oldestSnapshot = 0;

		BIT_SET(Mtm->disabledNodeMask, nodeId);
		Mtm->nConfigChanges += 1;
		Mtm->nAllNodes += 1;
		MtmUnlock();

		MtmStartReceiver(nodeId+1, true);
	}
	PG_RETURN_VOID();
}

Datum
mtm_poll_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	bool nowait = PG_GETARG_BOOL(1);
	bool online = true;
	while ((nodeId == MtmNodeId && Mtm->status != MTM_ONLINE)
		   || (nodeId != MtmNodeId && BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)))
	{
		if (nowait) {
			online = false;
			break;
		} else {
			MtmSleep(STATUS_POLL_DELAY);
		}
	}
	if (!nowait) {
		/* Just wait some time until logical repication channels will be reestablished */
		MtmSleep(MSEC_TO_USEC(MtmNodeDisableDelay));
	}
	PG_RETURN_BOOL(online);
}

Datum
mtm_recover_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	MtmRecoverNode(nodeId);
	PG_RETURN_VOID();
}

Datum
mtm_resume_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	MtmResumeNode(nodeId);
	PG_RETURN_VOID();
}

Datum
mtm_get_snapshot(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(MtmTx.snapshot);
}


Datum
mtm_get_last_csn(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(Mtm->lastCsn);
}

Datum
mtm_get_csn(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_INT64(0);
	MtmTransState* ts;
	csn_t csn = INVALID_CSN;

	MtmLock(LW_SHARED);
	ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	if (ts != NULL) {
		csn = ts->csn;
	}
	MtmUnlock();

	return csn;
}

typedef struct
{
	int		  nodeId;
	TupleDesc desc;
	Datum	  values[Natts_mtm_nodes_state];
	bool	  nulls[Natts_mtm_nodes_state];
} MtmGetNodeStateCtx;

Datum
mtm_get_nodes_state(PG_FUNCTION_ARGS)
{
	FuncCallContext* funcctx;
	MtmGetNodeStateCtx* usrfctx;
	MemoryContext oldcontext;
	int64 lag;
	bool is_first_call = SRF_IS_FIRSTCALL();

	if (is_first_call) {
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		usrfctx = (MtmGetNodeStateCtx*)palloc(sizeof(MtmGetNodeStateCtx));
		get_call_result_type(fcinfo, NULL, &usrfctx->desc);
		usrfctx->nodeId = 1;
		memset(usrfctx->nulls, false, sizeof(usrfctx->nulls));
		funcctx->user_fctx = usrfctx;
		MemoryContextSwitchTo(oldcontext);
	}
	funcctx = SRF_PERCALL_SETUP();
	usrfctx = (MtmGetNodeStateCtx*)funcctx->user_fctx;
	if (usrfctx->nodeId > Mtm->nAllNodes) {
		SRF_RETURN_DONE(funcctx);
	}
	usrfctx->values[0] = Int32GetDatum(usrfctx->nodeId);
	usrfctx->values[1] = BoolGetDatum(!BIT_CHECK(Mtm->disabledNodeMask, usrfctx->nodeId-1));
	usrfctx->values[2] = BoolGetDatum(!BIT_CHECK(SELF_CONNECTIVITY_MASK, usrfctx->nodeId-1));
	usrfctx->values[3] = BoolGetDatum(BIT_CHECK(Mtm->stalledNodeMask, usrfctx->nodeId-1));
	usrfctx->values[4] = BoolGetDatum(BIT_CHECK(Mtm->stoppedNodeMask, usrfctx->nodeId-1));

	usrfctx->values[5] = BoolGetDatum(BIT_CHECK(Mtm->originLockNodeMask, usrfctx->nodeId-1));
	lag = MtmGetSlotLag(usrfctx->nodeId);
	usrfctx->values[6] = Int64GetDatum(lag);
	usrfctx->nulls[6] = lag < 0;

	usrfctx->values[7] = Int64GetDatum(Mtm->transCount ? Mtm->nodes[usrfctx->nodeId-1].transDelay/Mtm->transCount : 0);
	usrfctx->values[8] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[usrfctx->nodeId-1].lastStatusChangeTime/USECS_PER_SEC));
	usrfctx->values[9] = Int64GetDatum(Mtm->nodes[usrfctx->nodeId-1].oldestSnapshot);

	usrfctx->values[10] = Int32GetDatum(Mtm->nodes[usrfctx->nodeId-1].senderPid);
	usrfctx->values[11] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[usrfctx->nodeId-1].senderStartTime/USECS_PER_SEC));
	usrfctx->values[12] = Int32GetDatum(Mtm->nodes[usrfctx->nodeId-1].receiverPid);
	usrfctx->values[13] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[usrfctx->nodeId-1].receiverStartTime/USECS_PER_SEC));

	if (usrfctx->nodeId == MtmNodeId)
	{
		usrfctx->nulls[10] = true;
		usrfctx->nulls[11] = true;
		usrfctx->nulls[12] = true;
		usrfctx->nulls[13] = true;
	}

	usrfctx->values[14] = CStringGetTextDatum(Mtm->nodes[usrfctx->nodeId-1].con.connStr);
	usrfctx->values[15] = Int64GetDatum(Mtm->nodes[usrfctx->nodeId-1].connectivityMask);
	usrfctx->values[16] = Int64GetDatum(Mtm->nodes[usrfctx->nodeId-1].nHeartbeats);
	usrfctx->nodeId += 1;

	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(usrfctx->desc, usrfctx->values, usrfctx->nulls)));
}

Datum
mtm_get_trans_by_gid(PG_FUNCTION_ARGS)
{
	TupleDesc desc;
	Datum	  values[Natts_mtm_trans_state];
	bool	  nulls[Natts_mtm_trans_state] = {false};
	MtmTransState* ts;
	MtmTransMap* tm;
	char *gid = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int i;

	MtmLock(LW_SHARED);
	tm = (MtmTransMap*)hash_search(MtmGid2State, gid, HASH_FIND, NULL);
	if (tm == NULL) {
		MtmUnlock();
		PG_RETURN_NULL();
	}

	values[1] = CStringGetTextDatum(gid);

	ts = tm->state;
	if (ts == NULL) {
		values[0] = CStringGetTextDatum(MtmTxnStatusMnem[tm->status]);
		for (i = 2; i < Natts_mtm_trans_state; i++) {
			nulls[i] = true;
		}
	} else {
		values[0] = CStringGetTextDatum(MtmTxnStatusMnem[ts->status]);
		values[2] = Int64GetDatum(ts->xid);
		values[3] = Int32GetDatum(ts->gtid.node);
		values[4] = Int64GetDatum(ts->gtid.xid);
		values[5] = TimestampTzGetDatum(time_t_to_timestamptz(ts->csn/USECS_PER_SEC));
		values[6] = TimestampTzGetDatum(time_t_to_timestamptz(ts->snapshot/USECS_PER_SEC));
		values[7] = BoolGetDatum(ts->isLocal);
		values[8] = BoolGetDatum(ts->isPrepared);
		values[9] = BoolGetDatum(ts->isActive);
		values[10] = BoolGetDatum(ts->isTwoPhase);
		values[11] = BoolGetDatum(ts->votingCompleted);
		values[12] = Int64GetDatum(ts->participantsMask);
		values[13] = Int64GetDatum(ts->votedMask);
		values[14] = Int32GetDatum(ts->nConfigChanges);
	}
	MtmUnlock();

	get_call_result_type(fcinfo, NULL, &desc);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

Datum
mtm_get_trans_by_xid(PG_FUNCTION_ARGS)
{
	TupleDesc desc;
	Datum	  values[Natts_mtm_trans_state];
	bool	  nulls[Natts_mtm_trans_state] = {false};
	TransactionId xid = PG_GETARG_INT64(0);
	MtmTransState* ts;

	MtmLock(LW_SHARED);
	ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	if (ts == NULL) {
		MtmUnlock();
		PG_RETURN_NULL();
	}

	values[0] = CStringGetTextDatum(MtmTxnStatusMnem[ts->status]);
	values[1] = CStringGetTextDatum(ts->gid);
	values[2] = Int64GetDatum(ts->xid);
	values[3] = Int32GetDatum(ts->gtid.node);
	values[4] = Int64GetDatum(ts->gtid.xid);
	values[5] = TimestampTzGetDatum(time_t_to_timestamptz(ts->csn/USECS_PER_SEC));
	values[6] = TimestampTzGetDatum(time_t_to_timestamptz(ts->snapshot/USECS_PER_SEC));
	values[7] = BoolGetDatum(ts->isLocal);
	values[8] = BoolGetDatum(ts->isPrepared);
	values[9] = BoolGetDatum(ts->isActive);
	values[10] = BoolGetDatum(ts->isTwoPhase);
	values[11] = BoolGetDatum(ts->votingCompleted);
	values[12] = Int64GetDatum(ts->participantsMask);
	values[13] = Int64GetDatum(ts->votedMask);
	MtmUnlock();

	get_call_result_type(fcinfo, NULL, &desc);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

Datum
mtm_get_cluster_state(PG_FUNCTION_ARGS)
{
	TupleDesc desc;
	Datum	  values[Natts_mtm_cluster_state];
	bool	  nulls[Natts_mtm_cluster_state] = {false};
	get_call_result_type(fcinfo, NULL, &desc);

	values[0] = Int32GetDatum(MtmNodeId);
	values[1] = CStringGetTextDatum(MtmNodeStatusMnem[Mtm->status]);
	values[2] = Int64GetDatum(Mtm->disabledNodeMask);
	values[3] = Int64GetDatum(SELF_CONNECTIVITY_MASK);
	values[4] = Int64GetDatum(Mtm->originLockNodeMask);
	values[5] = Int32GetDatum(Mtm->nLiveNodes);
	values[6] = Int32GetDatum(Mtm->nAllNodes);
	values[7] = Int32GetDatum((int)Mtm->pool.active);
	values[8] = Int32GetDatum((int)Mtm->pool.pending);
	values[9] = Int64GetDatum(BgwPoolGetQueueSize(&Mtm->pool));
	values[10] = Int64GetDatum(Mtm->transCount);
	values[11] = Int64GetDatum(Mtm->timeShift);
	values[12] = Int32GetDatum(Mtm->recoverySlot);
	values[13] = Int64GetDatum(hash_get_num_entries(MtmXid2State));
	values[14] = Int64GetDatum(hash_get_num_entries(MtmGid2State));
	values[15] = Int64GetDatum(Mtm->oldestXid);
	values[16] = Int32GetDatum(Mtm->nConfigChanges);
	values[17] = Int64GetDatum(Mtm->stalledNodeMask);
	values[18] = Int64GetDatum(Mtm->stoppedNodeMask);
	values[19] = Int64GetDatum(Mtm->deadNodeMask);
	values[20] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[MtmNodeId-1].lastStatusChangeTime/USECS_PER_SEC));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}


typedef struct
{
	int		  nodeId;
} MtmGetClusterInfoCtx;

static void erase_option_from_connstr(const char *option, char *connstr)
{
	char *needle = psprintf("%s=", option);
	while (1) {
		char *found = strstr(connstr, needle);
		if (found == NULL) break;
		while (*found != '\0' && *found != ' ') {
			*found = ' ';
			found++;
		}
	}
	pfree(needle);
}

PGconn *
PQconnectdb_safe(const char *conninfo, int timeout)
{
	PGconn *conn;
	struct timeval tv = { timeout, 0 };
	char *safe_connstr = pstrdup(conninfo);

	/* XXXX add timeout to connstring if set */

	erase_option_from_connstr("arbiter_port", safe_connstr);
	conn = PQconnectdb(safe_connstr);

	if (PQstatus(conn) != CONNECTION_OK)
	{
		MTM_ELOG(WARNING, "Could not connect to '%s': %s",
			safe_connstr, PQerrorMessage(conn));
		return conn;
	}

	pfree(safe_connstr);

	if (timeout != 0)
	{
		int socket_fd = PQsocket(conn);

		if (socket_fd < 0)
		{
			MTM_ELOG(WARNING, "Referee socket is invalid");
			return conn;
		}

		if (pg_setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
									(char *)&tv, sizeof(tv), MtmUseRDMA) < 0)
		{
			MTM_ELOG(WARNING, "Could not set referee socket timeout: %s",
						strerror(errno));
			return conn;
		}
	}

	return conn;
}

Datum
mtm_collect_cluster_info(PG_FUNCTION_ARGS)
{

	FuncCallContext* funcctx;
	MtmGetClusterInfoCtx* usrfctx;
	MemoryContext oldcontext;
	TupleDesc desc;
	bool is_first_call = SRF_IS_FIRSTCALL();
	int i;
	PGconn* conn;
	PGresult *result;
	char* values[Natts_mtm_cluster_state];
	HeapTuple tuple;

	if (is_first_call) {
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		usrfctx = (MtmGetClusterInfoCtx*)palloc(sizeof(MtmGetNodeStateCtx));
		get_call_result_type(fcinfo, NULL, &desc);
		funcctx->attinmeta = TupleDescGetAttInMetadata(desc);
		usrfctx->nodeId = 0;
		funcctx->user_fctx = usrfctx;
		MemoryContextSwitchTo(oldcontext);
	}
	funcctx = SRF_PERCALL_SETUP();
	usrfctx = (MtmGetClusterInfoCtx*)funcctx->user_fctx;
	while (++usrfctx->nodeId <= Mtm->nAllNodes && BIT_CHECK(Mtm->disabledNodeMask, usrfctx->nodeId-1));
	if (usrfctx->nodeId > Mtm->nAllNodes) {
		SRF_RETURN_DONE(funcctx);
	}

	conn = PQconnectdb_safe(Mtm->nodes[usrfctx->nodeId-1].con.connStr, 0);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		MTM_ELOG(WARNING, "Failed to establish connection '%s' to node %d: error = %s", Mtm->nodes[usrfctx->nodeId-1].con.connStr, usrfctx->nodeId, PQerrorMessage(conn));
		PQfinish(conn);
		SRF_RETURN_NEXT_NULL(funcctx);
	}
	else
	{
		result = PQexec(conn, "select * from mtm.get_cluster_state()");

		if (PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1) {
			MTM_ELOG(ERROR, "Failed to receive data from %d", usrfctx->nodeId);
		}

		for (i = 0; i < Natts_mtm_cluster_state; i++) {
			values[i] = PQgetvalue(result, 0, i);
		}
		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
		PQclear(result);
		PQfinish(conn);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
}

Datum mtm_broadcast_table(PG_FUNCTION_ARGS)
{
	MtmCopyRequest copy;
	copy.sourceTable = PG_GETARG_OID(0);
	copy.targetNodes = PG_GETARG_INT64(1);
	LogLogicalMessage("B", (char*)&copy, sizeof(copy), true);
	MtmTx.containsDML = true;
	PG_RETURN_VOID();
}

Datum mtm_copy_table(PG_FUNCTION_ARGS)
{
	MtmCopyRequest copy;
	copy.sourceTable = PG_GETARG_OID(0);
	copy.targetNodes = (nodemask_t)1 << (PG_GETARG_INT32(1) - 1);
	LogLogicalMessage("B", (char*)&copy, sizeof(copy), true);
	MtmTx.containsDML = true;
	PG_RETURN_VOID();
}


Datum mtm_make_table_local(PG_FUNCTION_ARGS)
{
	Oid	reloid = PG_GETARG_OID(0);
	RangeVar   *rv;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Datum		values[Natts_mtm_local_tables];
	bool		nulls[Natts_mtm_local_tables];

	MtmMakeRelationLocal(reloid);

	rv = makeRangeVar(MULTIMASTER_SCHEMA_NAME, MULTIMASTER_LOCAL_TABLES_TABLE, -1);
	rel = heap_openrv(rv, RowExclusiveLock);
	if (rel != NULL) {
		char* tableName = get_rel_name(reloid);
		Oid	  schemaid = get_rel_namespace(reloid);
		char* schemaName = get_namespace_name(schemaid);

		tupDesc = RelationGetDescr(rel);

		/* Form a tuple. */
		memset(nulls, false, sizeof(nulls));

		values[Anum_mtm_local_tables_rel_schema - 1] = CStringGetDatum(schemaName);
		values[Anum_mtm_local_tables_rel_name - 1] = CStringGetDatum(tableName);

		tup = heap_form_tuple(tupDesc, values, nulls);

		/* Insert the tuple to the catalog. */
		simple_heap_insert(rel, tup);

		/* Update the indexes. */
		CatalogUpdateIndexes(rel, tup);

		/* Cleanup. */
		heap_freetuple(tup);
		heap_close(rel, RowExclusiveLock);

		MtmTx.containsDML = true;
	}
	return false;
}

Datum mtm_dump_lock_graph(PG_FUNCTION_ARGS)
{
	StringInfo s = makeStringInfo();
	int i;
	for (i = 0; i < Mtm->nAllNodes; i++)
	{
		size_t lockGraphSize;
		char  *lockGraphData;
		MtmLockNode(i + 1 + MtmMaxNodes, LW_SHARED);
		lockGraphSize = Mtm->nodes[i].lockGraphUsed;
		lockGraphData = palloc(lockGraphSize);
		memcpy(lockGraphData, Mtm->nodes[i].lockGraphData, lockGraphSize);
		MtmUnlockNode(i + 1 + MtmMaxNodes);

		if (lockGraphData) {
			GlobalTransactionId *gtid = (GlobalTransactionId *) lockGraphData;
			GlobalTransactionId *last = (GlobalTransactionId *) (lockGraphData + lockGraphSize);
			appendStringInfo(s, "node-%d lock graph: ", i+1);
			while (gtid != last) {
				GlobalTransactionId *src = gtid++;
				appendStringInfo(s, "%d:%llu -> ", src->node, (long64)src->xid);
				while (gtid->node != 0) {
					GlobalTransactionId *dst = gtid++;
					appendStringInfo(s, "%d:%llu, ", dst->node, (long64)dst->xid);
				}
				gtid += 1;
			}
			appendStringInfo(s, "\n");
		}
	}
	return CStringGetTextDatum(s->data);
}

Datum mtm_inject_2pc_error(PG_FUNCTION_ARGS)
{
	Mtm->inject2PCError = PG_GETARG_INT32(0);
	PG_RETURN_VOID();
}

/*
 * -------------------------------------------
 * Broadcast utulity statements
 * -------------------------------------------
 */

/*
 * Execute statement with specified parameters and check its result
 */
static bool MtmRunUtilityStmt(PGconn* conn, char const* sql, char **errmsg)
{
	PGresult *result = PQexec(conn, sql);
	int status = PQresultStatus(result);

	bool ret = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;

	if (!ret) {
		char *errstr = PQresultErrorMessage(result);
		int errlen = strlen(errstr);
		if (errlen > 9) {
			*errmsg = palloc0(errlen);

			/* Strip "ERROR:  " from beginning and "\n" from end of error string */
			strncpy(*errmsg, errstr + 8, errlen - 1 - 8);
		}
	}

	PQclear(result);
	return ret;
}

static void
MtmNoticeReceiver(void *i, const PGresult *res)
{
	char *notice = PQresultErrorMessage(res);
	char *stripped_notice;
	int len = strlen(notice);

	/* Skip notices from other nodes */
	if ( (*(int *)i) != MtmNodeId - 1)
		return;

	stripped_notice = palloc0(len + 1);

	if (*notice == 'N')
	{
		/* Strip "NOTICE:  " from beginning and "\n" from end of error string */
		strncpy(stripped_notice, notice + 9, len - 1 - 9);
		MTM_ELOG(NOTICE, "%s", stripped_notice);
	}
	else if (*notice == 'W')
	{
		/* Strip "WARNING:	" from beginning and "\n" from end of error string */
		strncpy(stripped_notice, notice + 10, len - 1 - 10);
		MTM_ELOG(WARNING, "%s", stripped_notice);
	}
	else
	{
		strncpy(stripped_notice, notice, len + 1);
		MTM_ELOG(WARNING, "%s", stripped_notice);
	}

	MTM_LOG1("%s", stripped_notice);
	pfree(stripped_notice);
}

static void MtmBroadcastUtilityStmt(char const* sql, bool ignoreError, int forceOnNode)
{
	int i = 0;
	nodemask_t disabledNodeMask = Mtm->disabledNodeMask;
	int failedNode = -1;
	char const* errorMsg = NULL;
	PGconn **conns = palloc0(sizeof(PGconn*)*Mtm->nAllNodes);
	char* utility_errmsg;
	int nNodes = Mtm->nAllNodes;

	for (i = 0; i < nNodes; i++)
	{
		if (!BIT_CHECK(disabledNodeMask, i) || (i + 1 == forceOnNode))
		{
			conns[i] = PQconnectdb_safe(psprintf("%s application_name=%s", Mtm->nodes[i].con.connStr, MULTIMASTER_BROADCAST_SERVICE), 0);
			if (PQstatus(conns[i]) != CONNECTION_OK)
			{
				if (ignoreError)
				{
					PQfinish(conns[i]);
					conns[i] = NULL;
				} else {
					failedNode = i;
					do {
						PQfinish(conns[i]);
					} while (--i >= 0);
					MTM_ELOG(ERROR, "Failed to establish connection '%s' to node %d, error = %s", Mtm->nodes[failedNode].con.connStr, failedNode+1, PQerrorMessage(conns[i]));
				}
			}
			PQsetNoticeReceiver(conns[i], MtmNoticeReceiver, &i);
		}
	}
	Assert(i == nNodes);

	for (i = 0; i < nNodes; i++)
	{
		if (conns[i])
		{
			if (!MtmRunUtilityStmt(conns[i], "BEGIN TRANSACTION", &utility_errmsg) && !ignoreError)
			{
				errorMsg = MTM_TAG "Failed to start transaction at node %d";
				failedNode = i;
				break;
			}
			if (!MtmRunUtilityStmt(conns[i], sql, &utility_errmsg) && !ignoreError)
			{
				if (i + 1 == MtmNodeId)
					errorMsg = psprintf(MTM_TAG "%s", utility_errmsg);
				else
				{
					MTM_ELOG(ERROR, "%s", utility_errmsg);
					errorMsg = MTM_TAG "Failed to run command at node %d";
				}

				failedNode = i;
				break;
			}
		}
	}
	if (failedNode >= 0 && !ignoreError)
	{
		for (i = 0; i < nNodes; i++)
		{
			if (conns[i])
			{
				MtmRunUtilityStmt(conns[i], "ROLLBACK TRANSACTION", &utility_errmsg);
			}
		}
	} else {
		for (i = 0; i < nNodes; i++)
		{
			if (conns[i] && !MtmRunUtilityStmt(conns[i], "COMMIT TRANSACTION", &utility_errmsg) && !ignoreError)
			{
				errorMsg = MTM_TAG "Commit failed at node %d";
				failedNode = i;
			}
		}
	}
	for (i = 0; i < nNodes; i++)
	{
		if (conns[i])
		{
			PQfinish(conns[i]);
		}
	}
	if (!ignoreError && failedNode >= 0)
	{
		elog(ERROR, errorMsg, failedNode+1);
	}
}

/*
 * Genenerate global transaction identifier for two-pahse commit.
 * It should be unique for all nodes
 */
static void
MtmGenerateGid(char* gid)
{
	static int localCount;
	sprintf(gid, "MTM-%d-%d-%d", MtmNodeId, MyProcPid, ++localCount);
}

/*
 * Replace normal commit with two-phase commit.
 * It is called either for commit of standalone command either for commit of transaction block.
 */
static bool MtmTwoPhaseCommit(MtmCurrentTrans* x)
{
	MTM_TXTRACE(x, "MtmTwoPhaseCommit Start");

	if (!x->isReplicated && x->isDistributed && x->containsDML) {
		MtmGenerateGid(x->gid);
		if (!x->isTransactionBlock) {
			BeginTransactionBlock(false);
			x->isTransactionBlock = true;
			CommitTransactionCommand();
			StartTransactionCommand();
		}
		if (!PrepareTransactionBlock(x->gid))
		{
			MTM_ELOG(WARNING, "Failed to prepare transaction %s (%llu)", x->gid, (long64)x->xid);
		} else {
			CommitTransactionCommand();
			StartTransactionCommand();
			if (x->isSuspended) {
				MTM_ELOG(WARNING, "Transaction %s (%llu) is left in prepared state because coordinator node is not online", x->gid, (long64)x->xid);
			} else {
				Assert(x->isActive);
				if (x->status == TRANSACTION_STATUS_ABORTED) {
					MtmTransState* ts;
					ts = (MtmTransState*) hash_search(MtmXid2State, &(x->xid), HASH_FIND, NULL);
					Assert(ts);

					TXFINISH("%s ABORT, MtmTwoPhase", x->gid);
					FinishPreparedTransaction(x->gid, false);
					MTM_ELOG(ERROR, "Transaction %s (%llu) is aborted on node %d. Check its log to see error details.", x->gid, (long64)x->xid, ts->abortedByNode);
				} else {
					TXFINISH("%s COMMIT, MtmTwoPhase", x->gid);
					FinishPreparedTransaction(x->gid, true);
					MTM_TXTRACE(x, "MtmTwoPhaseCommit Committed");
					MTM_LOG2("Distributed transaction %s (%lld) is committed at %lld with LSN=%lld", x->gid, (long64)x->xid, MtmGetCurrentTime(), (long64)GetXLogInsertRecPtr());
				}
			}
		}
		return true;
	}
	return false;
}


/*
 * -------------------------------------------
 * GUC Context Handling
 * -------------------------------------------
 */

// XXX: is it defined somewhere?
#define GUC_KEY_MAXLEN 255
#define MTM_GUC_HASHSIZE 20

typedef struct MtmGucEntry
{
	char	key[GUC_KEY_MAXLEN];
	dlist_node	list_node;
	char   *value;
} MtmGucEntry;

static HTAB *MtmGucHash = NULL;
static dlist_head MtmGucList = DLIST_STATIC_INIT(MtmGucList);
static inline void MtmGucUpdate(const char *key, char *value);

static void MtmGucInit(void)
{
	HASHCTL		hash_ctl;
	char	   *current_role;
	MemoryContext oldcontext;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = GUC_KEY_MAXLEN;
	hash_ctl.entrysize = sizeof(MtmGucEntry);
	hash_ctl.hcxt = TopMemoryContext;
	MtmGucHash = hash_create("MtmGucHash",
						MTM_GUC_HASHSIZE,
						&hash_ctl,
						HASH_ELEM | HASH_CONTEXT);

	/*
	 * If current role is not equal to MtmDatabaseUser, than set it bofore
	 * any other GUC vars.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	current_role = GetConfigOptionByName("session_authorization", NULL, false);
	if (current_role && *current_role && strcmp(MtmDatabaseUser, current_role) != 0)
		MtmGucUpdate("session_authorization", current_role);
	MemoryContextSwitchTo(oldcontext);
}

static void MtmGucDiscard()
{
	dlist_iter iter;

	if (dlist_is_empty(&MtmGucList))
		return;

	dlist_foreach(iter, &MtmGucList)
	{
		MtmGucEntry *cur_entry = dlist_container(MtmGucEntry, list_node, iter.cur);
		pfree(cur_entry->value);
	}
	dlist_init(&MtmGucList);

	hash_destroy(MtmGucHash);
	MtmGucHash = NULL;
}

static inline void MtmGucUpdate(const char *key, char *value)
{
	MtmGucEntry *hentry;
	bool found;

	if (!MtmGucHash)
		MtmGucInit();

	hentry = (MtmGucEntry*)hash_search(MtmGucHash, key, HASH_ENTER, &found);
	if (found)
	{
		pfree(hentry->value);
		dlist_delete(&hentry->list_node);
	}
	hentry->value = value;
	dlist_push_tail(&MtmGucList, &hentry->list_node);
}

static inline void MtmGucRemove(const char *key)
{
	MtmGucEntry *hentry;
	bool found;

	if (!MtmGucHash)
		MtmGucInit();

	hentry = (MtmGucEntry*)hash_search(MtmGucHash, key, HASH_FIND, &found);
	if (found)
	{
		pfree(hentry->value);
		dlist_delete(&hentry->list_node);
		hash_search(MtmGucHash, key, HASH_REMOVE, NULL);
	}
}

static void MtmGucSet(VariableSetStmt *stmt, const char *queryStr)
{
	MemoryContext oldcontext;

	if (!MtmGucHash)
		MtmGucInit();

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
			MtmGucUpdate(stmt->name, ExtractSetVariableArgs(stmt));
			break;

		case VAR_SET_DEFAULT:
			MtmGucRemove(stmt->name);
			break;

		case VAR_RESET:
			if (strcmp(stmt->name, "session_authorization") == 0)
				MtmGucRemove("role");
			MtmGucRemove(stmt->name);
			break;

		case VAR_RESET_ALL:
			/* XXX: shouldn't we keep auth/role here? */
			MtmGucDiscard();
			break;

		case VAR_SET_CURRENT:
		case VAR_SET_MULTI:
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

char* MtmGucSerialize(void)
{
	StringInfo serialized_gucs;
	dlist_iter iter;
	const char *search_path;

	if (!MtmGucHash)
		MtmGucInit();

	serialized_gucs = makeStringInfo();

	dlist_foreach(iter, &MtmGucList)
	{
		MtmGucEntry *cur_entry = dlist_container(MtmGucEntry, list_node, iter.cur);

		if (strcmp(cur_entry->key, "search_path") == 0)
			continue;

		appendStringInfoString(serialized_gucs, "SET ");
		appendStringInfoString(serialized_gucs, cur_entry->key);
		appendStringInfoString(serialized_gucs, " TO ");

		/* quite a crutch */
		if (strstr(cur_entry->key, "_mem") != NULL || *(cur_entry->value) == '\0')
		{
			appendStringInfoString(serialized_gucs, "'");
			appendStringInfoString(serialized_gucs, cur_entry->value);
			appendStringInfoString(serialized_gucs, "'");
		}
		else
		{
			appendStringInfoString(serialized_gucs, cur_entry->value);
		}
		appendStringInfoString(serialized_gucs, "; ");
	}

	/*
	 * Crutch for scheduler. It sets search_path through SetConfigOption()
	 * so our callback do not react on that.
	 */
	search_path = GetConfigOption("search_path", false, true);
	appendStringInfo(serialized_gucs, "SET search_path TO %s; ", search_path);

	return serialized_gucs->data;
}

/*
 * -------------------------------------------
 * DDL Handling
 * -------------------------------------------
 */

static void MtmProcessDDLCommand(char const* queryString, bool transactional)
{
	if (MtmTx.isReplicated)
		return;

	if (transactional)
	{
		char *gucCtx = MtmGucSerialize();
		queryString = psprintf("RESET SESSION AUTHORIZATION; reset all; %s %s", gucCtx, queryString);

		/* Transactional DDL */
		MTM_LOG3("Sending DDL: %s", queryString);
		LogLogicalMessage("D", queryString, strlen(queryString) + 1, true);
		MtmTx.containsDML = true;
	}
	else
	{
		/* Concurrent DDL */
		MTM_LOG1("Sending concurrent DDL: %s", queryString);
		XLogFlush(LogLogicalMessage("C", queryString, strlen(queryString) + 1, false));
	}
}

static void MtmFinishDDLCommand()
{
	LogLogicalMessage("E", "", 1, true);
}

void MtmUpdateLockGraph(int nodeId, void const* messageBody, int messageSize)
{
	int allocated;
	MtmLockNode(nodeId + MtmMaxNodes, LW_EXCLUSIVE);
	allocated = Mtm->nodes[nodeId-1].lockGraphAllocated;
	if (messageSize > allocated) {
		allocated = Max(Max(MULTIMASTER_LOCK_BUF_INIT_SIZE, allocated*2), messageSize);
		Mtm->nodes[nodeId-1].lockGraphData = ShmemAlloc(allocated);
		if (Mtm->nodes[nodeId-1].lockGraphData == NULL) {
			elog(PANIC, "Failed to allocate shared memory for lock graph: %d bytes requested",
				 allocated);
		}
		Mtm->nodes[nodeId-1].lockGraphAllocated = allocated;
	}
	memcpy(Mtm->nodes[nodeId-1].lockGraphData, messageBody, messageSize);
	Mtm->nodes[nodeId-1].lockGraphUsed = messageSize;
	MtmUnlockNode(nodeId + MtmMaxNodes);
	MTM_LOG1("Update deadlock graph for node %d size %d", nodeId, messageSize);
}

static bool MtmIsTempType(TypeName* typeName)
{
	bool isTemp = false;

	if (typeName != NULL)
	{
		Type typeTuple = LookupTypeName(NULL, typeName, NULL, false);
		if (typeTuple != NULL)
		{
			Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);
		    Oid relid = typeStruct->typrelid;
		    ReleaseSysCache(typeTuple);

			if (relid != InvalidOid)
			{
				HeapTuple classTuple = SearchSysCache1(RELOID, relid);
				Form_pg_class classStruct = (Form_pg_class) GETSTRUCT(classTuple);
				if (classStruct->relpersistence == 't')
					isTemp = true;
				ReleaseSysCache(classTuple);
			}
		}
	}
	return isTemp;
}

static bool MtmFunctionProfileDependsOnTempTable(CreateFunctionStmt* func)
{
	ListCell* elem;

	if (MtmIsTempType(func->returnType))
	{
		return true;
	}
	foreach (elem, func->parameters)
	{
		FunctionParameter* param = (FunctionParameter*) lfirst(elem);
		if (MtmIsTempType(param->argType))
		{
			return true;
		}
	}
	return false;
}



static void MtmProcessUtility(Node *parsetree, const char *queryString,
							  ProcessUtilityContext context, ParamListInfo params,
							  DestReceiver *dest, char *completionTag)
{
	bool skipCommand = false;
	bool executed = false;
	bool prevMyXactAccessedTempRel;

	MTM_LOG2("%d: Process utility statement tag=%d, context=%d, issubtrans=%d, creating_extension=%d, query=%s",
			 MyProcPid, nodeTag(parsetree), context, IsSubTransaction(), creating_extension, queryString);
	switch (nodeTag(parsetree))
	{
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;
				switch (stmt->kind)
				{
				case TRANS_STMT_BEGIN:
				case TRANS_STMT_START:
					MtmTx.isTransactionBlock = true;
					break;
				case TRANS_STMT_COMMIT:
					if (MtmTwoPhaseCommit(&MtmTx)) {
						return;
					}
					break;
				case TRANS_STMT_PREPARE:
					MtmTx.isTwoPhase = true;
					strcpy(MtmTx.gid, stmt->gid);
					break;
				case TRANS_STMT_COMMIT_PREPARED:
				case TRANS_STMT_ROLLBACK_PREPARED:
					Assert(!MtmTx.isTwoPhase);
					strcpy(MtmTx.gid, stmt->gid);
					break;
				default:
					break;
				}
			}
			/* no break */
		case T_PlannedStmt:
		case T_ClosePortalStmt:
		case T_FetchStmt:
		case T_DoStmt:
		case T_CommentStmt:
		case T_PrepareStmt:
		case T_ExecuteStmt:
		case T_DeallocateStmt:
		case T_NotifyStmt:
		case T_ListenStmt:
		case T_UnlistenStmt:
		case T_LoadStmt:
		case T_ClusterStmt:
		case T_VariableShowStmt:
		case T_ReassignOwnedStmt:
		case T_LockStmt: // XXX: check whether we should replicate that
		case T_CheckPointStmt:
		case T_ReindexStmt:
		case T_ExplainStmt:
		case T_AlterSystemStmt:
			skipCommand = true;
			break;

		case T_CreatedbStmt:
		case T_DropdbStmt:
			elog(ERROR, "Multimaster doesn't support creating and dropping databases");
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
			{
				if (MtmApplyContext != NULL)
				{
					MemoryContext oldContext = MemoryContextSwitchTo(MtmApplyContext);
					Assert(oldContext != MtmApplyContext);
					MtmTablespaceStmt = copyObject(parsetree);
					MemoryContextSwitchTo(oldContext);
					return;
				}
				else
				{
					skipCommand = true;
					MtmProcessDDLCommand(queryString, false);
				}
			}
			break;

		case T_VacuumStmt:
		{
			VacuumStmt* vacuum = (VacuumStmt*)parsetree;
			skipCommand = true;
			if ((vacuum->options & VACOPT_LOCAL) == 0 && !MtmVolksWagenMode)
			{
				if (context == PROCESS_UTILITY_TOPLEVEL) {
					MtmProcessDDLCommand(queryString, false);
					MtmTx.isDistributed = false;
				} else if (MtmApplyContext != NULL) {
					MemoryContext oldContext = MemoryContextSwitchTo(MtmApplyContext);
					Assert(oldContext != MtmApplyContext);
					MtmVacuumStmt = (VacuumStmt*)copyObject(parsetree);
					MemoryContextSwitchTo(oldContext);
					return;
				}
			}
			break;
		}
		case T_CreateDomainStmt:
			/* Detect temp tables access */
			{
				CreateDomainStmt *stmt = (CreateDomainStmt *) parsetree;
				HeapTuple	typeTup;
				Form_pg_type baseType;
				Form_pg_type elementType;
				Form_pg_class pgClassStruct;
				int32		basetypeMod;
				Oid			elementTypeOid;
				Oid			tableOid;
				HeapTuple pgClassTuple;
				HeapTuple elementTypeTuple;

				typeTup = typenameType(NULL, stmt->typeName, &basetypeMod);
				baseType = (Form_pg_type) GETSTRUCT(typeTup);
				elementTypeOid = baseType->typelem;
				ReleaseSysCache(typeTup);

				if (elementTypeOid == InvalidOid)
					break;

				elementTypeTuple = SearchSysCache1(TYPEOID, elementTypeOid);
				elementType = (Form_pg_type) GETSTRUCT(elementTypeTuple);
				tableOid = elementType->typrelid;
				ReleaseSysCache(elementTypeTuple);

				if (tableOid == InvalidOid)
					break;

				pgClassTuple = SearchSysCache1(RELOID, tableOid);
				pgClassStruct = (Form_pg_class) GETSTRUCT(pgClassTuple);
				if (pgClassStruct->relpersistence == 't')
					MyXactAccessedTempRel = true;
				ReleaseSysCache(pgClassTuple);
			}
			break;

		// case T_ExplainStmt:
		//	/*
		//	 * EXPLAIN ANALYZE can create side-effects.
		//	 * Better to catch that by some general mechanism of detecting
		//	 * catalog and heap writes.
		//	 */
		//	{
		//		ExplainStmt *stmt = (ExplainStmt *) parsetree;
		//		ListCell   *lc;

		//		skipCommand = true;
		//		foreach(lc, stmt->options)
		//		{
		//			DefElem	   *opt = (DefElem *) lfirst(lc);
		//			if (strcmp(opt->defname, "analyze") == 0)
		//				skipCommand = false;
		//		}
		//	}
		//	break;

		/* Save GUC context for consequent DDL execution */
		case T_DiscardStmt:
			{
				DiscardStmt *stmt = (DiscardStmt *) parsetree;

				if (!IsTransactionBlock() && stmt->target == DISCARD_ALL)
				{
					skipCommand = true;
					MtmGucDiscard();
				}
			}
			break;
		case T_VariableSetStmt:
			{
				VariableSetStmt *stmt = (VariableSetStmt *) parsetree;

				/* Prevent SET TRANSACTION from replication */
				if (stmt->kind == VAR_SET_MULTI)
					skipCommand = true;

				if (!IsTransactionBlock())
				{
					skipCommand = true;
					MtmGucSet(stmt, queryString);
				}
			}
			break;

		case T_IndexStmt:
			{
				IndexStmt *indexStmt = (IndexStmt *) parsetree;
				if (indexStmt->concurrent)
				{
					 if (context == PROCESS_UTILITY_TOPLEVEL) {
						 MtmProcessDDLCommand(queryString, false);
						 MtmTx.isDistributed = false;
						 skipCommand = true;
						 /*
						  * Index is created at replicas completely asynchronously, so to prevent unintended interleaving with subsequent
						  * commands in this session, just wait here for a while.
						  * It will help to pass regression tests but will not be enough for construction of real large indexes
						  * where difference between completion of this operation at different nodes is unlimited
						  */
						 MtmSleep(USECS_PER_SEC);
					 } else if (MtmApplyContext != NULL) {
						 MemoryContext oldContext = MemoryContextSwitchTo(MtmApplyContext);
						 Assert(oldContext != MtmApplyContext);
						 MtmIndexStmt = (IndexStmt*)copyObject(indexStmt);
						 MemoryContextSwitchTo(oldContext);
						 return;
					 }
				}
			}
			break;

		case T_TruncateStmt:
			skipCommand = false;
			// MtmLockCluster();
			break;

		case T_DropStmt:
			{
				DropStmt *stmt = (DropStmt *) parsetree;
				if (stmt->removeType == OBJECT_INDEX && stmt->concurrent)
				{
					if (context == PROCESS_UTILITY_TOPLEVEL) {
						MtmProcessDDLCommand(queryString, false);
						MtmTx.isDistributed = false;
						skipCommand = true;
					} else if (MtmApplyContext != NULL) {
						 MemoryContext oldContext = MemoryContextSwitchTo(MtmApplyContext);
						 Assert(oldContext != MtmApplyContext);
						 MtmDropStmt = (DropStmt*)copyObject(stmt);
						 MemoryContextSwitchTo(oldContext);
						 return;
					}
				}
				else if (stmt->removeType == OBJECT_FUNCTION && MtmTx.isReplicated)
				{
					/* Make it possible to drop functions which were not replicated */
					stmt->missing_ok = true;
				}
			}
			break;

		/* Copy need some special care */
		case T_CopyStmt:
		{
			CopyStmt *copyStatement = (CopyStmt *) parsetree;
			skipCommand = true;
			if (copyStatement->is_from) {
				ListCell *opt;
				RangeVar *relation = copyStatement->relation;

				if (relation != NULL)
				{
					Oid relid = RangeVarGetRelid(relation, NoLock, true);
					if (OidIsValid(relid))
					{
						Relation rel = heap_open(relid, ShareLock);
						if (RelationNeedsWAL(rel)) {
							MtmTx.containsDML = true;
						}
						heap_close(rel, ShareLock);
					}
				}

				foreach(opt, copyStatement->options)
				{
					DefElem	*elem = lfirst(opt);
					if (strcmp("local", elem->defname) == 0) {
						MtmTx.isDistributed = false; /* Skip */
						MtmTx.snapshot = INVALID_CSN;
						MtmTx.containsDML = false;
						break;
					}
				}
			}
		    case T_CreateFunctionStmt:
		    {
				if (MtmTx.isReplicated)
				{
					// disable functiob body cehck at replica
					check_function_bodies = false;
				}
			}
			break;
		}

		default:
			skipCommand = false;
			break;
	}

	if (!skipCommand && !MtmTx.isReplicated && !MtmDDLStatement)
	{
		MTM_LOG3("Process DDL statement '%s', MtmTx.isReplicated=%d, MtmIsLogicalReceiver=%d", queryString, MtmTx.isReplicated, MtmIsLogicalReceiver);
		MtmProcessDDLCommand(queryString, true);
		executed = true;
		MtmDDLStatement = queryString;
	}
	else MTM_LOG3("Skip utility statement '%s': skip=%d, insideDDL=%d", queryString, skipCommand, MtmDDLStatement != NULL);

	prevMyXactAccessedTempRel = MyXactAccessedTempRel;

	if (PreviousProcessUtilityHook != NULL)
	{
		PreviousProcessUtilityHook(parsetree, queryString, context,
								   params, dest, completionTag);
	}
	else
	{
		standard_ProcessUtility(parsetree, queryString, context,
								params, dest, completionTag);
	}
#if 0
	if (!MtmVolksWagenMode && MtmTx.isDistributed && XactIsoLevel != XACT_REPEATABLE_READ) {
		MTM_ELOG(ERROR, "Isolation level %s is not supported by multimaster", isoLevelStr[XactIsoLevel]);
	}
#endif
	/* Allow replication of functions operating on temporary tables.
	 * Even through temporary table doesn't exist at replica, diasabling functoin body check makes it possible to create such function at replica.
	 * And it can be accessed later at replica if correspondent temporary table will be created.
	 * But disable replication of functions returning temporary tables: such functions can not be created at replica in any case.
	 */
	if (IsA(parsetree, CreateFunctionStmt))
	{
		if (MtmFunctionProfileDependsOnTempTable((CreateFunctionStmt*)parsetree))
		{
			prevMyXactAccessedTempRel = true;
		}
		MyXactAccessedTempRel = prevMyXactAccessedTempRel;
	}
	if (MyXactAccessedTempRel)
	{
		MTM_LOG1("Xact accessed temp table, stopping replication of statement '%s'", queryString);
		MtmTx.isDistributed = false; /* Skip */
		MtmTx.snapshot = INVALID_CSN;
	}

	if (executed)
	{
		MtmFinishDDLCommand();
		MtmDDLStatement = NULL;
	}
	if (IsA(parsetree, CreateStmt))
	{
		CreateStmt* create = (CreateStmt*)parsetree;
		Oid relid = RangeVarGetRelid(create->relation, NoLock, true);
		if (relid != InvalidOid) {
			Oid constraint_oid;
			Bitmapset* pk = get_primary_key_attnos(relid, true, &constraint_oid);
			if (pk == NULL && !MtmVolksWagenMode) {
				elog(WARNING,
					 MtmIgnoreTablesWithoutPk
					 ? "Table %s.%s without primary will not be replicated"
					 : "Updates and deletes of table %s.%s without primary will not be replicated",
					 create->relation->schemaname ? create->relation->schemaname : "public",
					 create->relation->relname);
			}
		}
	}
}

static void
MtmExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (!MtmTx.isReplicated && !MtmDDLStatement)
	{
		ListCell   *tlist;

		if (!MtmRemoteFunctions)
		{
			MtmInitializeRemoteFunctionsMap();
		}

		foreach(tlist, queryDesc->plannedstmt->planTree->targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlist);
			if (tle->expr && IsA(tle->expr, FuncExpr))
			{
				Oid func_oid = ((FuncExpr*)tle->expr)->funcid;
				if (!hash_search(MtmRemoteFunctions, &func_oid, HASH_FIND, NULL))
				{
					Form_pg_proc funcform;
					bool is_sec_def;
					HeapTuple func_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
					if (!HeapTupleIsValid(func_tuple))
						elog(ERROR, "cache lookup failed for function %u", func_oid);
					funcform = (Form_pg_proc) GETSTRUCT(func_tuple);
					is_sec_def = funcform->prosecdef;
					ReleaseSysCache(func_tuple);
					if (!is_sec_def)
					{
						continue;
					}
				}
				/*
				 * Execute security defined functions or functions marked as remote at replicated nodes.
				 * Them are executed as DDL statements.
				 * All data modifications done inside this function are not replicated.
				 * As a result generated content can vary at different nodes.
				 */
				MtmProcessDDLCommand(queryDesc->sourceText, true);
				MtmDDLStatement = queryDesc;
				break;
			}
		}
	}
	if (PreviousExecutorStartHook != NULL)
		PreviousExecutorStartHook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
MtmExecutorFinish(QueryDesc *queryDesc)
{
	/*
	 * If tx didn't wrote to XLOG then there is nothing to commit on other nodes.
	 */
	if (MtmDoReplication) {
		CmdType operation = queryDesc->operation;
		EState *estate = queryDesc->estate;
		if (estate->es_processed != 0 && (operation == CMD_INSERT || operation == CMD_UPDATE || operation == CMD_DELETE)) {
			int i;
			for (i = 0; i < estate->es_num_result_relations; i++) {
				Relation rel = estate->es_result_relations[i].ri_RelationDesc;
				if (RelationNeedsWAL(rel)) {
					if (MtmIgnoreTablesWithoutPk) {
						if (!rel->rd_indexvalid) {
							RelationGetIndexList(rel);
						}
						if (rel->rd_replidindex == InvalidOid) {
							MtmMakeRelationLocal(RelationGetRelid(rel));
							continue;
						}
					}
					MTM_LOG3("MtmTx.containsDML = true // WAL");
					MtmTx.containsDML = true;
					break;
				}
			}
		}
	}

	if (PreviousExecutorFinishHook != NULL)
	{
		PreviousExecutorFinishHook(queryDesc);
	}
	else
	{
		standard_ExecutorFinish(queryDesc);
	}

	if (MtmDDLStatement == queryDesc)
	{
		MtmFinishDDLCommand();
		MtmDDLStatement = NULL;
	}
}

static void MtmSeqNextvalHook(Oid seqid, int64 next)
{
	if (MtmMonotonicSequences)
	{
		MtmSeqPosition pos;
		pos.seqid = seqid;
		pos.next = next;
		LogLogicalMessage("N", (char*)&pos, sizeof(pos), true);
	}
}

/*
 * -------------------------------------------
 * Executor pool interface
 * -------------------------------------------
 */

void MtmExecute(void* work, int size)
{
	if (Mtm->status == MTM_RECOVERY) {
		/* During recovery apply changes sequentially to preserve commit order */
		MtmExecutor(work, size);
	} else {
		BgwPoolExecute(&Mtm->pool, work, size);
	}
}

static BgwPool*
MtmPoolConstructor(void)
{
	return &Mtm->pool;
}

/*
 * -------------------------------------------
 * Deadlock detection
 * -------------------------------------------
 */

static void
MtmGetGtid(TransactionId xid, GlobalTransactionId* gtid)
{
	MtmTransState* ts;

	MtmLock(LW_SHARED);
	ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	if (ts != NULL) {
		*gtid = ts->gtid;
	} else {
		gtid->node = MtmNodeId;
		gtid->xid = xid;
	}
	MtmUnlock();
}


static void
MtmSerializeLock(PROCLOCK* proclock, void* arg)
{
	ByteBuffer* buf = (ByteBuffer*)arg;
	LOCK* lock = proclock->tag.myLock;
	PGPROC* proc = proclock->tag.myProc;
	GlobalTransactionId gtid;
	if (lock != NULL) {
		PGXACT* srcPgXact = &ProcGlobal->allPgXact[proc->pgprocno];

		if (TransactionIdIsValid(srcPgXact->xid) && proc->waitLock == lock) {
			LockMethod lockMethodTable = GetLocksMethodTable(lock);
			int numLockModes = lockMethodTable->numLockModes;
			int conflictMask = lockMethodTable->conflictTab[proc->waitLockMode];
			SHM_QUEUE *procLocks = &(lock->procLocks);
			int lm;

			MtmGetGtid(srcPgXact->xid, &gtid);	/* waiting transaction */

			ByteBufferAppend(buf, &gtid, sizeof(gtid));

			proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
												 offsetof(PROCLOCK, lockLink));
			while (proclock)
			{
				if (proc != proclock->tag.myProc) {
					PGXACT* dstPgXact = &ProcGlobal->allPgXact[proclock->tag.myProc->pgprocno];
					if (TransactionIdIsValid(dstPgXact->xid)) {
						Assert(srcPgXact->xid != dstPgXact->xid);
						for (lm = 1; lm <= numLockModes; lm++)
						{
							if ((proclock->holdMask & LOCKBIT_ON(lm)) && (conflictMask & LOCKBIT_ON(lm)))
							{
								MTM_LOG3("%d: %u(%u) waits for %u(%u)", MyProcPid, srcPgXact->xid, proc->pid, dstPgXact->xid, proclock->tag.myProc->pid);
								MtmGetGtid(dstPgXact->xid, &gtid); /* transaction holding lock */
								ByteBufferAppend(buf, &gtid, sizeof(gtid));
								break;
							}
						}
					}
				}
				proclock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->lockLink,
													 offsetof(PROCLOCK, lockLink));
			}
			gtid.node = 0;
			gtid.xid = 0;
			ByteBufferAppend(buf, &gtid, sizeof(gtid)); /* end of lock owners list */
		}
	}
}

static bool
MtmDetectGlobalDeadLockForXid(TransactionId xid)
{
	bool hasDeadlock = false;
	if (TransactionIdIsValid(xid)) {
		ByteBuffer buf;
		MtmGraph graph;
		GlobalTransactionId gtid;
		int i;

		ByteBufferAlloc(&buf);
		EnumerateLocks(MtmSerializeLock, &buf);

		Assert(replorigin_session_origin == InvalidRepOriginId);
		XLogFlush(LogLogicalMessage("L", buf.data, buf.used, false));

		MtmGraphInit(&graph);
		MtmGraphAdd(&graph, (GlobalTransactionId*)buf.data, buf.used/sizeof(GlobalTransactionId));
		ByteBufferFree(&buf);
		for (i = 0; i < Mtm->nAllNodes; i++) {
			if (i+1 != MtmNodeId && !BIT_CHECK(Mtm->disabledNodeMask, i)) {
				size_t lockGraphSize;
				void* lockGraphData;
				MtmLockNode(i + 1 + MtmMaxNodes, LW_SHARED);
				lockGraphSize = Mtm->nodes[i].lockGraphUsed;
				lockGraphData = palloc(lockGraphSize);
				memcpy(lockGraphData, Mtm->nodes[i].lockGraphData, lockGraphSize);
				MtmUnlockNode(i + 1 + MtmMaxNodes);

				if (lockGraphData == NULL) {
					return true;
				} else {
					MtmGraphAdd(&graph, (GlobalTransactionId*)lockGraphData, lockGraphSize/sizeof(GlobalTransactionId));
				}
			}
		}
		MtmGetGtid(xid, &gtid);
		hasDeadlock = MtmGraphFindLoop(&graph, &gtid);
		MTM_ELOG(LOG, "Distributed deadlock check by backend %d for %u:%llu = %d", MyProcPid, gtid.node, (long64)gtid.xid, hasDeadlock);
		if (!hasDeadlock) {
			/* There is no deadlock loop in graph, but deadlock can be caused by lack of apply workers: if all of them are busy, then some transactions
			 * can not be appied just because there are no vacant workers and it cause additional dependency between transactions which is not
			 * refelected in lock graph
			 */
			timestamp_t lastPeekTime = BgwGetLastPeekTime(&Mtm->pool);
			if (lastPeekTime != 0 && MtmGetSystemTime() - lastPeekTime >= MSEC_TO_USEC(DeadlockTimeout)) {
				hasDeadlock = true;
				MTM_ELOG(WARNING, "Apply workers were blocked more than %d msec",
					 (int)USEC_TO_MSEC(MtmGetSystemTime() - lastPeekTime));
			} else {
				MTM_LOG1("Enable deadlock timeout in backend %d for transaction %llu", MyProcPid, (long64)xid);
				enable_timeout_after(DEADLOCK_TIMEOUT, DeadlockTimeout);
			}
		}
	}
	return hasDeadlock;
}

static bool
MtmDetectGlobalDeadLock(PGPROC* proc)
{
	PGXACT* pgxact = &ProcGlobal->allPgXact[proc->pgprocno];

	MTM_LOG1("Detect global deadlock for %llu by backend %d", (long64)pgxact->xid, MyProcPid);

	return MtmDetectGlobalDeadLockForXid(pgxact->xid);
}

Datum mtm_check_deadlock(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_INT64(0);
	PG_RETURN_BOOL(MtmDetectGlobalDeadLockForXid(xid));
}

Datum mtm_referee_poll(PG_FUNCTION_ARGS)
{
	nodemask_t recoveredNodeMask;

	MtmLock(LW_EXCLUSIVE);
	recoveredNodeMask = Mtm->recoveredNodeMask;
	Mtm->deadNodeMask = PG_GETARG_INT64(0);
	Mtm->recoveredNodeMask &= ~Mtm->deadNodeMask;
	// XXXX: FIXME: put some event here
	// MtmCheckQuorum();
	MtmUnlock();

	PG_RETURN_INT64(recoveredNodeMask);
}

/*
 * Allow to replicate handcrafted heap inserts/updates.
 * Needed for scheduler.
 */
void
MtmToggleDML(void)
{
	MtmTx.containsDML = true;
}

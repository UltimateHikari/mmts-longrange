#include "postgres.h"
#include "miscadmin.h" /* PostmasterPid */
#include "multimaster.h"
#include "state.h"
#include "executor/spi.h"
#include "utils/snapmgr.h"
#include "nodes/makefuncs.h"
#include "catalog/namespace.h"

char const* const MtmNeighborEventMnem[] =
{
	"MTM_NEIGHBOR_CLIQUE_DISABLE",
	"MTM_NEIGHBOR_WAL_RECEIVER_START",
	"MTM_NEIGHBOR_WAL_SENDER_START_RECOVERY",
	"MTM_NEIGHBOR_WAL_SENDER_START_RECOVERED",
	"MTM_NEIGHBOR_RECOVERY_CAUGHTUP"
};

char const* const MtmEventMnem[] =
{
	"MTM_REMOTE_DISABLE",
	"MTM_CLIQUE_DISABLE",
	"MTM_CLIQUE_MINORITY",
	"MTM_ARBITER_RECEIVER_START",
	"MTM_RECOVERY_START1",
	"MTM_RECOVERY_START2",
	"MTM_RECOVERY_FINISH1",
	"MTM_RECOVERY_FINISH2",
	"MTM_NONRECOVERABLE_ERROR"
};

static int  MtmRefereeGetWinner(void);
static bool MtmRefereeClearWinner(void);

static bool mtm_state_initialized;

// XXXX: allocate in context and clean it
static char *
maskToString(nodemask_t mask, int nNodes)
{
	char *strMask = palloc0(nNodes + 1);
	int i;

	for (i = 0; i < nNodes; i++)
		strMask[i] = BIT_CHECK(mask, i) ? '1' : '0';

	return strMask;
}

int
countZeroBits(nodemask_t mask, int nNodes)
{
	int i, count = 0;
	for (i = 0; i < nNodes; i++)
	{
		if (!BIT_CHECK(mask, i))
			count++;
	}
	return count;
}

static void
MtmSetClusterStatus(MtmNodeStatus status)
{
	if (Mtm->status == status)
		return;

	Mtm->nConfigChanges += 1; /* this will restart backends */

	MTM_LOG1("[STATE]   Switching status from %s to %s status",
		MtmNodeStatusMnem[Mtm->status], MtmNodeStatusMnem[status]);

	/*
	 * Do some actions on specific status transitions.
	 * This will be executed only once because of preceeding if stmt.
	 */
	if (status == MTM_DISABLED)
	{
		Mtm->recoverySlot = 0;
		Mtm->pglogicalReceiverMask = 0;
		Mtm->pglogicalSenderMask = 0;
		Mtm->recoveryCount++; /* this will restart replication connection */
	}

	Mtm->status = status;
}

static void
MtmCheckState(void)
{
	// int nVotingNodes = MtmGetNumberOfVotingNodes();
	bool isEnabledState;
	MtmNodeStatus old_status;
	int nEnabled   = countZeroBits(Mtm->disabledNodeMask, Mtm->nAllNodes);
	int nConnected = countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes);
	int nReceivers = Mtm->nAllNodes - countZeroBits(Mtm->pglogicalReceiverMask, Mtm->nAllNodes);
	int nSenders   = Mtm->nAllNodes - countZeroBits(Mtm->pglogicalSenderMask, Mtm->nAllNodes);

	old_status = Mtm->status;

	MTM_LOG1("[STATE]   Status = (disabled=%s, unaccessible=%s, clique=%s, receivers=%s, senders=%s, total=%i, major=%d, stopped=%s)",
		maskToString(Mtm->disabledNodeMask, Mtm->nAllNodes),
		maskToString(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes),
		maskToString(Mtm->clique, Mtm->nAllNodes),
		maskToString(Mtm->pglogicalReceiverMask, Mtm->nAllNodes),
		maskToString(Mtm->pglogicalSenderMask, Mtm->nAllNodes),
		Mtm->nAllNodes,
		(MtmMajorNode || Mtm->refereeGrant),
		maskToString(Mtm->stoppedNodeMask, Mtm->nAllNodes));

	isEnabledState =
		( (nConnected >= Mtm->nAllNodes/2+1)							/* majority */
			// XXXX: should we restrict major with two nodes setup?
			|| (nConnected == Mtm->nAllNodes/2 && MtmMajorNode)			/* or half + major node */
			|| (nConnected == Mtm->nAllNodes/2 && Mtm->refereeGrant) )  /* or half + referee */
		&& (BIT_CHECK(Mtm->clique, MtmNodeId-1) || Mtm->refereeGrant)	/* in clique when non-major */
		&& !BIT_CHECK(Mtm->stoppedNodeMask, MtmNodeId-1);				/* is not stopped */

	/* ANY -> MTM_DISABLED */
	if (!isEnabledState)
	{
		// BIT_SET(Mtm->disabledNodeMask, MtmNodeId-1);
		MtmSetClusterStatus(MTM_DISABLED);
		MtmDisableNode(MtmNodeId);
		return;
	}

	switch (Mtm->status)
	{
		case MTM_DISABLED:
			if (isEnabledState)
			{
				MtmSetClusterStatus(MTM_RECOVERY);

				if (old_status != Mtm->status)
					MtmCheckState();
				return;
			}
			break;

		case MTM_RECOVERY:
			if (!BIT_CHECK(Mtm->disabledNodeMask, MtmNodeId-1))
			{
				MTM_LOG1("[LOCK] set lock on MTM_RECOVERY switch");
				BIT_SET(Mtm->originLockNodeMask, MtmNodeId-1); // kk trick, XXXX: log that
				MtmSetClusterStatus(MTM_RECOVERED);

				if (old_status != Mtm->status)
					MtmCheckState();
				return;
			}
			break;

		/*
		 * Switching from MTM_RECOVERY to MTM_ONLINE requires two state
		 * re-checks. If by the time of MTM_RECOVERY -> MTM_RECOVERED all
		 * senders/receiveirs already atarted we can stuck in MTM_RECOVERED
		 * state. Hence call MtmCheckState() from periodic status check while
		 * in MTM_RECOVERED state.
		 */
		case MTM_RECOVERED:
			if (nReceivers == nEnabled-1 && nSenders == nEnabled-1 && nEnabled == nConnected)
			{
				/*
				 * It should be already cleaned by RECOVERY_CAUGHTUP, but
				 * in major mode or with referee we can be working alone
				 * so nobody will clean it.
				 */
				MTM_LOG1("[LOCK] release lock on MTM_RECOVERED switch");
				BIT_CLEAR(Mtm->originLockNodeMask, MtmNodeId-1);
				MtmSetClusterStatus(MTM_ONLINE);

				if (old_status != Mtm->status)
					MtmCheckState();
				return;
			}
			break;

		case MTM_ONLINE:
			break;
	}

}


void
MtmStateProcessNeighborEvent(int node_id, MtmNeighborEvent ev) // XXXX camelcase node_id
{
	MTM_LOG1("[STATE] Node %i: %s", node_id, MtmNeighborEventMnem[ev]);

	Assert(node_id != MtmNodeId);

	MtmLock(LW_EXCLUSIVE);
	switch(ev)
	{
		case MTM_NEIGHBOR_CLIQUE_DISABLE:
			MtmDisableNode(node_id);
			break;

		case MTM_NEIGHBOR_WAL_RECEIVER_START:
			BIT_SET(Mtm->pglogicalReceiverMask, node_id - 1);
			MTM_LOG1("[LOCK] release lock on MTM_NEIGHBOR_WAL_RECEIVER_START event");
			BIT_CLEAR(Mtm->originLockNodeMask, MtmNodeId-1);
			break;

		case MTM_NEIGHBOR_WAL_SENDER_START_RECOVERY:
			/*
			 * With big heartbeat recv timeout it can heppend that other node will
			 * restart faster than we can detect that. Without disabledNodeMask bit set
			 * we will never send recovery_finish in such case. So set it now.
			 *
			 * It is also possible to change logic of recovery_finish but for
			 * now it is easier to do it here. MtmIsRecoveredNode deserves rewrite anyway.
			 */
			if (!BIT_CHECK(Mtm->disabledNodeMask, node_id-1))
			{
				BIT_SET(Mtm->disabledNodeMask, node_id-1);
			}
			// BIT_SET(Mtm->pglogicalSenderMask, node_id - 1);
			break;

		case MTM_NEIGHBOR_WAL_SENDER_START_RECOVERED:
			BIT_SET(Mtm->pglogicalSenderMask, node_id - 1);
			MtmEnableNode(node_id); /// XXXX ?
			break;

		case MTM_NEIGHBOR_RECOVERY_CAUGHTUP:
			MTM_LOG1("[LOCK] release lock on MTM_NEIGHBOR_RECOVERY_CAUGHTUP event");
			BIT_CLEAR(Mtm->originLockNodeMask, node_id - 1);
			MtmEnableNode(node_id);
			break;

	}
	MtmCheckState();
	MtmUnlock();
}


void
MtmStateProcessEvent(MtmEvent ev)
{
	MTM_LOG1("[STATE] %s", MtmEventMnem[ev]);

	MtmLock(LW_EXCLUSIVE);
	switch (ev)
	{
		case MTM_CLIQUE_DISABLE:
			BIT_SET(Mtm->disabledNodeMask, MtmNodeId-1);
			Mtm->recoveryCount++; /* this will restart replication connection */
			break;

		case MTM_REMOTE_DISABLE:
		case MTM_CLIQUE_MINORITY:
			break;

		case MTM_ARBITER_RECEIVER_START:
			MtmOnNodeConnect(MtmNodeId);
			break;

		case MTM_RECOVERY_START1:
		case MTM_RECOVERY_START2:
			break;

		case MTM_RECOVERY_FINISH1:
		case MTM_RECOVERY_FINISH2:
			{
				int i;

				MtmEnableNode(MtmNodeId);

				Mtm->recoveryCount++; /* this will restart replication connection */

				Mtm->recoverySlot = 0;
				Mtm->recoveredLSN = GetXLogInsertRecPtr();
				Mtm->nConfigChanges += 1;
				for (i = 0; i < Mtm->nAllNodes; i++)
					Mtm->nodes[i].lastHeartbeat = 0; /* defuse watchdog until first heartbeat is received */
			}
			break;

		case MTM_NONRECOVERABLE_ERROR:
			// kill(PostmasterPid, SIGQUIT);
			break;
	}

	MtmCheckState();
	MtmUnlock();

}

/*
 * Node is disabled if it is not part of clique built using connectivity masks of all nodes.
 * There is no warranty that all nodes will make the same decision about clique, but as far as we want to avoid
 * some global coordinator (which will be SPOF), we have to rely on Bron–Kerbosch algorithm locating maximum clique in graph
 */
void MtmDisableNode(int nodeId)
{
	MTM_LOG1("[STATE] Node %i: disabled", nodeId);

	BIT_SET(Mtm->disabledNodeMask, nodeId-1);
	Mtm->nConfigChanges += 1;
	Mtm->nodes[nodeId-1].timeline += 1;
	Mtm->nodes[nodeId-1].lastStatusChangeTime = MtmGetSystemTime();
	Mtm->nodes[nodeId-1].lastHeartbeat = 0; /* defuse watchdog until first heartbeat is received */

	if (Mtm->status == MTM_ONLINE) {
		/* Make decision about prepared transaction status only in quorum */
		MtmLock(LW_EXCLUSIVE);
		MtmPollStatusOfPreparedTransactionsForDisabledNode(nodeId, false);
		MtmUnlock();
	}
}


/*
 * Node is enabled when it's recovery is completed.
 * This why node is mostly marked as recovered when logical sender/receiver to this node is (re)started.
 */
void MtmEnableNode(int nodeId)
{
	MTM_LOG1("[STATE] Node %i: enabled", nodeId);

	if (BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)) {
		BIT_CLEAR(Mtm->disabledNodeMask, nodeId-1);
		BIT_CLEAR(Mtm->reconnectMask, nodeId-1);
		BIT_SET(Mtm->recoveredNodeMask, nodeId-1);
		Mtm->nConfigChanges += 1;
		Mtm->nodes[nodeId-1].lastStatusChangeTime = MtmGetSystemTime();
		Mtm->nodes[nodeId-1].lastHeartbeat = 0; /* defuse watchdog until first heartbeat is received */
		if (nodeId != MtmNodeId) {
			Mtm->nLiveNodes += 1;
		}
	}
}

/*
 *
 */

void MtmOnNodeDisconnect(int nodeId)
{
	if (BIT_CHECK(SELF_CONNECTIVITY_MASK, nodeId-1))
		return;

	MTM_LOG1("[STATE] Node %i: disconnected", nodeId);

	/*
	 * We should disable it, as clique detector will not necessarily
	 * do that. For example it will anyway find clique with one node.
	 */

	MtmLock(LW_EXCLUSIVE);
	MtmDisableNode(nodeId);
	BIT_SET(SELF_CONNECTIVITY_MASK, nodeId-1);
	BIT_SET(Mtm->reconnectMask, nodeId-1);
	Mtm->nConfigChanges += 1;
	MtmCheckState();
	MtmUnlock();

	// MtmRefreshClusterStatus();
}

// XXXX: make that event too
void MtmOnNodeConnect(int nodeId)
{
	// if (!BIT_CHECK(SELF_CONNECTIVITY_MASK, nodeId-1))
	// 	return;

	MTM_LOG1("[STATE] Node %i: connected", nodeId);

	MtmLock(LW_EXCLUSIVE);
	BIT_CLEAR(SELF_CONNECTIVITY_MASK, nodeId-1);
	BIT_SET(Mtm->reconnectMask, nodeId-1);

	MtmCheckState();
	MtmUnlock();

	// MtmRefreshClusterStatus();
}

void MtmReconnectNode(int nodeId) // XXXX evict that
{
	// MTM_LOG1("[STATE] ReconnectNode for node %u", nodeId);
	MtmLock(LW_EXCLUSIVE);
	BIT_SET(Mtm->reconnectMask, nodeId-1);
	MtmUnlock();
}


/**
 * Build internode connectivity mask. 1 - means that node is disconnected.
 */
static void
MtmBuildConnectivityMatrix(nodemask_t* matrix)
{
	int i, j, n = Mtm->nAllNodes;

	for (i = 0; i < n; i++)
		matrix[i] = Mtm->nodes[i].connectivityMask | Mtm->deadNodeMask;

	/* make matrix symmetric: required for Bron–Kerbosch algorithm */
	for (i = 0; i < n; i++) {
		for (j = 0; j < i; j++) {
			matrix[i] |= ((matrix[j] >> i) & 1) << j;
			matrix[j] |= ((matrix[i] >> j) & 1) << i;
		}
		matrix[i] &= ~((nodemask_t)1 << i);
	}
}



/**
 * Build connectivity graph, find clique in it and extend disabledNodeMask by nodes not included in clique.
 * This function is called by arbiter monitor process with period MtmHeartbeatSendTimeout
 */
void
MtmRefreshClusterStatus()
{
	nodemask_t newClique, oldClique;
	nodemask_t matrix[MAX_NODES];
	nodemask_t trivialClique = ~SELF_CONNECTIVITY_MASK & (((nodemask_t)1 << Mtm->nAllNodes)-1);
	int cliqueSize;
	int i;

	/*
	 * Periodical check that we are still in RECOVERED state.
	 * See comment to MTM_RECOVERED -> MTM_ONLINE transition in MtmCheckState()
	 */
	MtmLock(LW_EXCLUSIVE);
	MtmCheckState();
	MtmUnlock();

	/*
	 * Check for referee decision when only half of nodes are visible.
	 * Do not hold lock here, but recheck later wheter mask changed.
	 */
	if (MtmRefereeConnStr && *MtmRefereeConnStr && !Mtm->refereeWinnerId &&
		countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes) == Mtm->nAllNodes/2)
	{
		int winner_node_id = MtmRefereeGetWinner();

		/* We also can have old value. Do that only from single mtm-monitor process */
		if (winner_node_id <= 0 && !mtm_state_initialized)
		{
			winner_node_id = MtmRefereeReadSaved();
			mtm_state_initialized = true;
		}

		if (winner_node_id > 0)
		{
			Mtm->refereeWinnerId = winner_node_id;
			if (!BIT_CHECK(SELF_CONNECTIVITY_MASK, winner_node_id - 1))
			{
				/*
				 * By the time we enter this block we can already see other nodes.
				 * So recheck old conditions under lock.
				 */
				MtmLock(LW_EXCLUSIVE);
				if (countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes) == Mtm->nAllNodes/2 &&
					!BIT_CHECK(SELF_CONNECTIVITY_MASK, winner_node_id - 1))
				{
					MTM_LOG1("[STATE] Referee allowed to proceed with half of the nodes (winner_id = %d)",
					winner_node_id);
					Mtm->refereeGrant = true;
					if (countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes) == 1)
					{
						// XXXX: that is valid for two nodes. Better idea is to parametrize MtmPollStatus*
						// functions.
						int neighbor_node_id = MtmNodeId == 1 ? 2 : 1;
						MtmPollStatusOfPreparedTransactionsForDisabledNode(neighbor_node_id, true);
					}
					MtmEnableNode(MtmNodeId);
					MtmCheckState();
				}
				MtmUnlock();
			}
		}
	}

	/*
	 * Clear winner if we again have all nodes recovered.
	 * We should clean old value based on disabledNodeMask instead of SELF_CONNECTIVITY_MASK
	 * because we can clean old value before failed node starts it recovery and that node
	 * can get refereeGrant before start of walsender, so it start in recovered mode.
	 */
	 if (MtmRefereeConnStr && *MtmRefereeConnStr && Mtm->refereeWinnerId &&
		countZeroBits(Mtm->disabledNodeMask, Mtm->nAllNodes) == Mtm->nAllNodes)
	{
		if (MtmRefereeClearWinner())
		{
			Mtm->refereeWinnerId = 0;
			Mtm->refereeGrant = false;
			MTM_LOG1("[STATE] Cleaning old referee decision");
		}
	}

	/*
	 * Check for clique.
	 */
	MtmBuildConnectivityMatrix(matrix);
	newClique = MtmFindMaxClique(matrix, Mtm->nAllNodes, &cliqueSize);

	if (newClique == Mtm->clique)
		return;

	MTM_LOG1("[STATE] Old clique: %s", maskToString(Mtm->clique, Mtm->nAllNodes));

	/*
	 * Otherwise make sure that all nodes have a chance to replicate their connectivity
	 * mask and we have the "consistent" picture. Obviously we can not get true consistent
	 * snapshot, but at least try to wait heartbeat send timeout is expired and
	 * connectivity graph is stabilized.
	 */
	do {
		oldClique = newClique;
		/*
		 * Double timeout to consider the worst case when heartbeat receive interval is added
		 * with refresh cluster status interval.
		 */
		MtmSleep(MSEC_TO_USEC(MtmHeartbeatRecvTimeout)*2);
		MtmBuildConnectivityMatrix(matrix);
		newClique = MtmFindMaxClique(matrix, Mtm->nAllNodes, &cliqueSize);
	} while (newClique != oldClique);

	MTM_LOG1("[STATE] New clique: %s", maskToString(oldClique, Mtm->nAllNodes));

	if (newClique != trivialClique)
	{
		MTM_LOG1("[STATE] NONTRIVIAL CLIQUE! (trivial: %s)", maskToString(trivialClique, Mtm->nAllNodes)); // XXXX some false-positives, fixme
	}

	/*
	 * We are using clique only to disable nodes.
	 * So find out what node should be disabled and disable them.
	 */
	MtmLock(LW_EXCLUSIVE);

	Mtm->clique = newClique;

	/*
	 * Do not perform any action based on clique with referee grant,
	 * because we can disable ourself.
	 * But we also need to maintain actual clique not disable ourselves
	 * when neighbour node will come back and we erase refereeGrant.
	 */
	if (Mtm->refereeGrant)
	{
		MtmUnlock();
		return;
	}

	for (i = 0; i < Mtm->nAllNodes; i++)
	{
		bool old_status = BIT_CHECK(Mtm->disabledNodeMask, i);
		bool new_status = BIT_CHECK(~newClique, i);

		if (new_status && new_status != old_status)
		{
			if ( i+1 == MtmNodeId )
				MtmStateProcessEvent(MTM_CLIQUE_DISABLE);
			else
				MtmStateProcessNeighborEvent(i+1, MTM_NEIGHBOR_CLIQUE_DISABLE);
		}
	}

	MtmCheckState();
	MtmUnlock();
}

/*
 * Referee caches decision in mtm.referee_decision
 */
static bool
MtmRefereeHasLocalTable()
{
	RangeVar   *rv;
	Oid			rel_oid;

	StartTransactionCommand();
	rv = makeRangeVar(MULTIMASTER_SCHEMA_NAME, "referee_decision", -1);
	rel_oid = RangeVarGetRelid(rv, NoLock, true);
	CommitTransactionCommand();

	return OidIsValid(rel_oid);
}

int
MtmRefereeReadSaved(void)
{
	int winner = -1;
	int rc;

	if (!MtmRefereeHasLocalTable())
		return -1;

	/* Save result locally */
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	rc = SPI_execute("select node_id from mtm.referee_decision where key = 'winner';", true, 0);
	if (rc != SPI_OK_SELECT)
	{
		MTM_ELOG(WARNING, "Failed to load referee decision");
	}
	else if (SPI_processed > 0)
	{
		bool isnull;
		winner = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
		Assert(SPI_processed == 1);
		Assert(!isnull);
	}
	else
	{
		/* no saved decision found */
		Assert(SPI_processed == 0);
	}
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	MTM_LOG1("Read saved referee decision, winner=%d.", winner);
	return winner;
}

static int
MtmRefereeGetWinner(void)
{
	PGconn* conn;
	PGresult *res;
	char sql[128];
	int  winner_node_id;
	int  old_winner = -1;
	int  rc;

	conn = PQconnectdb_safe(MtmRefereeConnStr, 5);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		MTM_ELOG(WARNING, "Could not connect to referee");
		PQfinish(conn);
		return -1;
	}

	sprintf(sql, "select referee.get_winner(%d)", MtmNodeId);
	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 ||
		PQnfields(res) != 1)
	{
		MTM_ELOG(WARNING, "Refusing unexpected result (r=%d, n=%d, w=%d, k=%s) from referee.get_winner()",
			PQresultStatus(res), PQntuples(res), PQnfields(res), PQgetvalue(res, 0, 0));
		PQclear(res);
		PQfinish(conn);
		return -1;
	}

	winner_node_id = atoi(PQgetvalue(res, 0, 0));

	if (winner_node_id < 1 || winner_node_id > Mtm->nAllNodes)
	{
		MTM_ELOG(WARNING,
			"Referee responded with node_id=%d, it's out of our node range",
			winner_node_id);
		PQclear(res);
		PQfinish(conn);
		return -1;
	}

	/* Ok, we finally got it! */
	PQclear(res);
	PQfinish(conn);

	/* Save result locally */
	if (MtmRefereeHasLocalTable())
	{
		MtmEnforceLocalTx = true;
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		/* Check old value if any */
		rc = SPI_execute("select node_id from mtm.referee_decision where key = 'winner';", true, 0);
		if (rc != SPI_OK_SELECT)
		{
			MTM_ELOG(WARNING, "Failed to load previous referee decision");
		}
		else if (SPI_processed > 0)
		{
			bool isnull;
			old_winner = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
			Assert(SPI_processed == 1);
			Assert(!isnull);
		}
		else
		{
			/* no saved decision found */
			Assert(SPI_processed == 0);
		}
		/* Update actual key */
		sprintf(sql,
			"insert into mtm.referee_decision values ('winner', %d) on conflict(key) do nothing;",
			winner_node_id);
		rc = SPI_execute(sql, false, 0);
		SPI_finish();
		if (rc < 0)
			MTM_ELOG(WARNING, "Failed to save referee decision, but proceeding anyway");
		PopActiveSnapshot();
		CommitTransactionCommand();
		MtmEnforceLocalTx = false;

		if (old_winner > 0 && old_winner != winner_node_id)
			MTM_LOG1("WARNING Overriding old referee decision (%d) with new one (%d)", old_winner, winner_node_id);
	}

	MTM_LOG1("Got referee response, winner node_id=%d.", winner_node_id);
	return winner_node_id;
}

static bool
MtmRefereeClearWinner(void)
{
	PGconn* conn;
	PGresult *res;
	char *response;
	int rc;

	/*
	 * Delete result locally first.
	 *
	 * If we delete decision from referee but fail to delete local cached
	 * that will be pretty bad -- on the next reboot we can read
	 * stale referee decision and on next failure end up with two masters.
	 * So just delete local cache first.
	 */
	if (MtmRefereeHasLocalTable())
	{
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		rc = SPI_execute("delete from mtm.referee_decision where key = 'winner'", false, 0);
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		if (rc < 0)
		{
			MTM_ELOG(WARNING, "Failed to save referee decision, proceeding anyway");
			return false;
		}
	}


	conn = PQconnectdb_safe(MtmRefereeConnStr, 5);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		MTM_ELOG(WARNING, "Could not connect to referee");
		PQfinish(conn);
		return false;
	}

	res = PQexec(conn, "select referee.clean()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 ||
		PQnfields(res) != 1)
	{
		MTM_ELOG(WARNING, "Refusing unexpected result (r=%d, n=%d, w=%d, k=%s) from referee.clean().",
			PQresultStatus(res), PQntuples(res), PQnfields(res), PQgetvalue(res, 0, 0));
		PQclear(res);
		PQfinish(conn);
		return false;
	}

	response = PQgetvalue(res, 0, 0);

	if (strncmp(response, "t", 1) != 0)
	{
		MTM_ELOG(WARNING, "Wrong response from referee.clean(): '%s'", response);
		PQclear(res);
		PQfinish(conn);
		return false;
	}

	/* Ok, we finally got it! */
	MTM_LOG1("Got referee clear response '%s'", response);
	PQclear(res);
	PQfinish(conn);
	return true;
}

/*
 * Mtm current status accessor.
 */
MtmNodeStatus
MtmGetCurrentStatus()
{
	volatile MtmNodeStatus status;
	MtmLock(LW_SHARED);
	status = Mtm->status;
	MtmUnlock();
	return status;
}

/*
 * Mtm current status accessor.
 */
nodemask_t
MtmGetDisabledNodeMask()
{
	volatile nodemask_t disabledMask;
	MtmLock(LW_SHARED);
	disabledMask = Mtm->disabledNodeMask;
	MtmUnlock();
	return disabledMask;
}

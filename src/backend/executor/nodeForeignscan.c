/*-------------------------------------------------------------------------
 *
 * nodeForeignscan.c
 *	  Support routines for sequential scans of foreign tables.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecForeignScan				sequentially scans a foreign table.
 *		ExecForeignNext				retrieve next tuple in sequential order.
 *		ExecInitForeignScan			creates and initializes a seqscan node.
 *		ExecEndForeignScan			releases any storage allocated.
 *		ExecForeignReScan			rescans the foreign table
 *		ExecForeignMarkPos			marks scan position
 *		ExecForeignRestrPos			restores scan position
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeForeignscan.h"
#include "foreign/foreign.h"
#include "miscadmin.h"

static TupleTableSlot *ForeignNext(ForeignScanState *node);
static bool ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ForeignNext
 *
 *		This is a workhorse for ExecForeignScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ForeignNext(ForeignScanState *node)
{
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	Assert(node->ss.ps.state->es_direction == ForwardScanDirection);

	/* tupleslot will be filled by Iterate. */
	if (node->routine->Iterate == NULL)
		ereport(ERROR,
				(errmsg("foreign-data wrapper must support Iterate to scan foreign table")));
	node->routine->Iterate(node);

	/* Set tableoid if the tuple was valid. */
	if (HeapTupleIsValid(slot->tts_tuple))
	{
		/*
		 * If the foreign-data wrapper returned a MinimalTuple, materialize the
		 * tuple to store system attributes.
		 */
		if (!TTS_HAS_PHYSICAL_TUPLE(slot))
			ExecMaterializeSlot(slot);

		/* overwrite only tableoid of the tuple */
		slot->tts_tuple->t_tableOid =
							RelationGetRelid(node->ss.ss_currentRelation);
	}

	return slot;
}

/*
 * ForeignRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
ForeignRecheck(ForeignScanState *node, TupleTableSlot *slot)
{
	/* ForeignScan never use keys in ForeignNext. */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecForeignScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecForeignScan(ForeignScanState *node)
{
	return ExecScan((ScanState *) node,
					(ExecScanAccessMtd) ForeignNext,
					(ExecScanRecheckMtd) ForeignRecheck);
}


/* ----------------------------------------------------------------
 *		ExecInitForeignScan
 * ----------------------------------------------------------------
 */
ForeignScanState *
ExecInitForeignScan(ForeignScan *node, EState *estate, int eflags)
{
	ForeignScanState   *scanstate;
	Relation			currentRelation;
	Oid					userid;
	FdwRoutine		   *routine;

	/*
	 * foreign scan has no child node.
	 * but not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(ForeignScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) scanstate);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ss.ps);
	ExecInitScanTupleSlot(estate, &scanstate->ss);

	/*
	 * initialize scan relation. get the relation object id from the
	 * relid'th entry in the range table, open that relation and acquire
	 * appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);
	scanstate->ss.ss_currentRelation = currentRelation;
	ExecAssignScanType(&scanstate->ss, RelationGetDescr(currentRelation));
	scanstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/* Initialize forein-data wrapper specific data. */
	userid = GetOuterUserId();
	scanstate->table = GetForeignTable(RelationGetRelid(currentRelation));
	scanstate->server = GetForeignServer(scanstate->table->serverid);
	scanstate->wrapper = GetForeignDataWrapper(scanstate->server->fdwid);
	scanstate->user = GetUserMapping(userid, scanstate->server->serverid);
	routine = GetFdwRoutine(scanstate->wrapper->fdwhandler);
	scanstate->routine = routine;

	/* Initialize the scan */
	if (routine->Open != NULL)
		routine->Open(scanstate);

	/*
	 * If this execution was not for EXPLAIN w/o ANALYZE flag, initiate the
	 * foreign scan.
	 */
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		/* connect to the foreign server and prepare to execute scan */
		if (routine->ConnectServer != NULL)
			scanstate->conn = routine->ConnectServer(scanstate->server,
													 scanstate->user);
		if (routine->BeginScan != NULL)
			routine->BeginScan(scanstate);
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndForeignScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndForeignScan(ForeignScanState *node)
{
	Relation		relation;

	/* close the scan */
	if (node->routine->Close != NULL)
		node->routine->Close(node);

	/* close the foreign connection for this scan node */
	if (node->routine->FreeFSConnection != NULL)
		node->routine->FreeFSConnection(node->conn);

	/* get information from node */
	relation = node->ss.ss_currentRelation;

	/* Free the exprcontext */
	ExecFreeExprContext(&node->ss.ps);

	/* clean out the tuple table */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* close the relation. */
	ExecCloseScanRelation(relation);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecForeignReScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecForeignReScan(ForeignScanState *node)
{
	if (node->routine->ReOpen != NULL)
		node->routine->ReOpen(node);

	ExecScanReScan((ScanState *) node);
}

/* ----------------------------------------------------------------
 *		ExecForeignMarkPos(node)
 *
 *		Marks scan position.
 * ----------------------------------------------------------------
 */
void
ExecForeignMarkPos(ForeignScanState *node)
{
	elog(ERROR, "ForeignScan does not support mark/restore");
}

/* ----------------------------------------------------------------
 *		ExecForeignRestrPos
 *
 *		Restores scan position.
 * ----------------------------------------------------------------
 */
void
ExecForeignRestrPos(ForeignScanState *node)
{
	elog(ERROR, "ForeignScan does not support mark/restore");
}

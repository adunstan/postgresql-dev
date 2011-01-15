/*-------------------------------------------------------------------------
 *
 * fdwapi.h
 *	  API for foreign-data wrappers
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * src/include/foreign/fdwapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWAPI_H
#define FDWAPI_H

#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"

/*
 * When a plan is going to be cached, the plan node is copied into another
 * context with copyObject. It means that FdwPlan, a part of ForeignScan plan
 * node, and its contents must have copyObject support too.
 */
struct FdwPlan
{
	NodeTag type;

	/*
	 * Free-form text shown in EXPLAIN. The SQL to be sent to the remote
	 * server is typically shown here.
	 */
	char *explainInfo;

	/*
	 * Cost estimation info. The startup_cost should include the cost of
	 * connecting to the remote host and sending over the query, as well as
	 * the cost of starting up the query so that it returns the first result
	 * row.
	 */
	double startup_cost;
	double total_cost;
#ifdef HOOK_ESTIMATE_REL_SIZE
	double rows;
	int width;
#endif

	/*
	 * FDW-private data. FDW must guarantee that every elements in this list
	 * have copyObject support.  If FDW needs to store arbitrary data such as
	 * non-Node structure, Const of bytea would be able to use as a container.
	 */
	List *private;
};
typedef struct FdwPlan FdwPlan;

struct FdwExecutionState
{
	/* FDW-private data */
	void *private;
};
typedef struct FdwExecutionState FdwExecutionState;

/*
 * Common interface routines of FDW, inspired by the FDW API in the SQL/MED
 * standard, but adapted to the PostgreSQL world.
 *
 * A foreign-data wrapper implements these routines. At a minimum, it must
 * implement BeginScan, Iterate and EndScan, and either PlanNative or
 * PlanRelScan.
 *
 * The PlanXXX functions return an FdwPlan struct that can later be executed
 * with BeginScan. The implementation should fill in the cost estimates in
 * FdwPlan, as well as a tuple descriptor that describes the result set.
 */
struct FdwRoutine
{
#ifdef IN_THE_FUTURE
	/*
	 * Plan a query of arbitrary native SQL (or other query language supported
	 * by the foreign server). This is used for SQL/MED passthrough mode, or
	 * e.g contrib/dblink.
	 */
	FdwPlan *(*PlanNative)(Oid serverid, char *query);

	/*
	 * Plan a whole subquery. This is used for example to execute an aggregate
	 * query remotely without pulling all the rows to the local server.
	 *
	 * The implementation can return NULL if it cannot satisfy the whole
	 * subquery, in which case the planner will break down the query into
	 * smaller parts and call PlanRelScan for the foreign tables involved.
	 *
	 * The implementation must be careful to only accept queries it fully
	 * understands! For example, if it ignores windowClauses, and returns
	 * a non-NULL results for a query that contains one, the windowClause
	 * would be lost and the query would return incorrect results.
	 */
	FdwPlan *(*PlanQuery)(PlannerInfo *root, Query query);
#endif

	/*
	 * Plan a scan on a foreign table. 'foreigntableid' identifies the foreign
	 * table, and 'attnos' is an integer list of attribute numbers for the
	 * columns to be returned. Note that 'attnos' can also be an empty list,
	 * typically for "SELECT COUNT(*) FROM foreigntable" style queries where
	 * we just need to know how many rows there are. The number and type of
	 * attributes in the tuple descriptor in the returned FdwPlan must match
	 * the attributes specified in attnos, or an error will be thrown.
	 *
	 * 'root' and 'baserel' contain context information that the
	 * implementation can use to restrict the rows that are fetched.
	 * baserel->baserestrictinfo is particularly interseting, as it contains
	 * quals (WHERE clauses) that can be used to filter the rows in the remote
	 * server. 'root' and 'baserel' can be safely ignored, the planner will
	 * re-check the quals on every fetched row anyway.
	 */
	FdwPlan *(*PlanRelScan)(Oid foreigntableid, PlannerInfo *root,
							RelOptInfo *baserel);

	/*
	 * Begin execution of a foreign scan.  This function is called when an
	 * actual scan is needed, so EXPLAIN without ANALYZE option doesn't call
	 * BeginScan().
	 */
	FdwExecutionState *(*BeginScan)(FdwPlan *plan, ParamListInfo params);

	/*
	 * Fetch the next record and store it into slot.
	 * Note that Iterate is called in per-tuple memory context which has been
	 * reset before each call.
	 */
	void (*Iterate)(FdwExecutionState *state, TupleTableSlot *slot);

	/*
	 * Reset the read pointer to the head of the scan.
	 * This function will be called when the new outer tuple was acquired in a
	 * nested loop.
	 */
	void (*ReScan)(FdwExecutionState *state);

	/*
	 * End the foreign scan and do clean up.
	 */
	void (*EndScan)(FdwExecutionState *state);
};
typedef struct FdwRoutine FdwRoutine;

#endif   /* FDWAPI_H */


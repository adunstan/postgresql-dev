/*-------------------------------------------------------------------------
 *
 * file_fdw.c
 *		  foreign-datga wrapper for server-side flat files.
 *
 * Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/file_fdw/file_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/reloptions.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * Describes the valid options for objects which uses this wrapper.
 */
struct FileFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Valid options for file_fdw.
 * These options are based on the options for COPY FROM command.
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileBeginScan().  See comments of the function for detail.
 */
static struct FileFdwOption valid_options[] = {
	/* File options */
	{ "filename",		ForeignTableRelationId },

	/* Format options */
	/* oids option is not supported */
	{ "format",			ForeignTableRelationId },
	{ "header",			ForeignTableRelationId },
	{ "delimiter",		ForeignTableRelationId },
	{ "quote",			ForeignTableRelationId },
	{ "escape",			ForeignTableRelationId },
	{ "null",			ForeignTableRelationId },

	/* FIXME: implement force_not_null option */

	/* Local option */
	{ "textarray",      ForeignTableRelationId },

	/* Sentinel */
	{ NULL,			InvalidOid }
};

/*
 * FDW-specific information for FdwExecutionState.
 */
typedef struct FileFdwPrivate {
	char		   *filename;
	bool            textarray;  /* make a text array rather than a tuple */
	Relation		rel;		/* scan target relation */
	CopyState		cstate;		/* state of read in file */
	List		   *options;	/* merged generic options, excluding filename */
} FileFdwPrivate;

/*
 * SQL functions
 */
extern Datum file_fdw_validator(PG_FUNCTION_ARGS);
extern Datum file_fdw_handler(PG_FUNCTION_ARGS);

/*
 * FDW routines
 */
static FdwPlan *filePlanRelScan(Oid foreigntableid, PlannerInfo *root,
								RelOptInfo *baserel);
static FdwExecutionState *fileBeginScan(FdwPlan *fplan, ParamListInfo params);
static void fileIterate(FdwExecutionState *festate, TupleTableSlot *slot);
static void fileEndScan(FdwExecutionState *festate);
static void fileReScan(FdwExecutionState *festate);

/* text array support */
static void makeTextArray(TupleTableSlot *slot, char **raw_fields, int nfields);
/*
 * Helper functions
 */
static char *generate_explain_info(const char *filename, unsigned long size);
static unsigned long estimate_costs(const char *filename, RelOptInfo *baserel,
									double *startup_cost, double *total_cost);

/*
 * Check if the provided option is one of valid options.
 * context is the Oid of the catalog the option came from, or 0 if we
 * don't care.
 */
static bool
is_valid_option(const char *option, Oid context)
{
	struct FileFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	return false;
}

/*
 * Validate the generic option given to FOREIGN DATA WRAPPER, SERVER, USER
 * MAPPING or FOREIGN TABLE which use file_fdw.
 * Raise an ERROR if the option or its value is considered
 * invalid.
 */
PG_FUNCTION_INFO_V1(file_fdw_validator);
Datum
file_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	ListCell   *cell;

	char	   *format = NULL;
	char	   *delimiter = NULL;
	char	   *quote = NULL;
	char	   *escape = NULL;
	char	   *null = NULL;
	bool		header;

	/* Only superuser can change generic options of the foreign table */
	if (catalog == ForeignTableRelationId && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can change foreign table options")));

	/* Validate each options */
	foreach(cell, options_list)
	{
		DefElem    *def = lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			struct FileFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
				errhint("Valid options in this context are: %s", buf.data)));

			PG_RETURN_BOOL(false);
		}

		if (strcmp(def->defname, "format") == 0)
		{
			if (pg_strcasecmp(strVal(def->arg), "csv") != 0 &&
				pg_strcasecmp(strVal(def->arg), "text") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("format must be csv or text")));
			format = strVal(def->arg);
		}
		else if (strcmp(def->defname, "header") == 0)
		{
			header = defGetBoolean(def);
		}
		else if (strcmp(def->defname, "delimiter") == 0)
		{
			if (strlen(strVal(def->arg)) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("delimiter must be a single one-byte1 character")));
			if (strchr(strVal(def->arg), '\r') != NULL ||
				strchr(strVal(def->arg), '\n') != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("delimiter cannot be newline or carriage return")));
			delimiter = strVal(def->arg);
		}
		else if (strcmp(def->defname, "quote") == 0)
		{
			if (strlen(strVal(def->arg)) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("quote must be 1 byte")));
			quote = strVal(def->arg);
		}
		else if (strcmp(def->defname, "escape") == 0)
		{
			if (strlen(strVal(def->arg)) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("escape must be 1 byte")));
			escape = strVal(def->arg);
		}
		else if (strcmp(def->defname, "null") == 0)
		{
			if (strchr(strVal(def->arg), '\r') != NULL ||
				strchr(strVal(def->arg), '\n') != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("null representation cannot use newline or carriage return")));
			null = strVal(def->arg);
		}
	}

	/* Check options which depend on the file format. */
	if (format != NULL && pg_strcasecmp(format, "text") == 0)
	{
		if (delimiter && strchr("\\.abcdefghijklmnopqrstuvwxyz0123456789",
				   delimiter[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
					 errmsg("delimiter cannot be \"%s\"", delimiter)));

		if (escape != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
					 errmsg("escape available only in CSV mode")));
	}
	else if (format != NULL && pg_strcasecmp(format, "csv") == 0)
	{
		if (null != NULL && quote != NULL && strchr(null, quote[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
					 errmsg("quote must not appear in the NULL specification")));
	}

	if (delimiter != NULL && quote != NULL)
		if (strcmp(delimiter, quote) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
					 errmsg("delimiter and quote must be different")));

	if (null != NULL && delimiter != NULL)
		if (strchr(null, delimiter[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
					 errmsg("delimiter must not appear in the NULL specification")));

	PG_RETURN_BOOL(true);
}

/*
 * return foreign-data wrapper handler object to execute foreign-data wrapper
 * routines.
 */
PG_FUNCTION_INFO_V1(file_fdw_handler);
Datum
file_fdw_handler(PG_FUNCTION_ARGS)
{
	static FdwRoutine file_fdw_routine =
	{
		filePlanRelScan,
		fileBeginScan,
		fileIterate,
		fileReScan,
		fileEndScan,
	};

	PG_RETURN_POINTER(&file_fdw_routine);
}

/*
 * Create a FdwPlan for a scan on the foreign table.
 *
 * FdwPlan must be able to be copied by copyObject(), so private area is a list
 * of copy-able elements.  The list consists of elements below:
 *
 *  (1) oid of the target relation, Oid Const
 *  (2) name of the file, String Value
 *  (3) list of fdw options excluding filename, List of DefElem
 *
 * This format must be used commonly in other planning functions, such as
 * PlanQuery and PlanNative.
 */
static FdwPlan *
filePlanRelScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *rel)
{
	Const		   *relid;
	Value		   *filename = NULL;
	bool            textarray = false;
	Const          *textarray_param;
	ulong			size;
	FdwPlan		   *fplan;
	ForeignTable   *table;
	ForeignServer  *server;
	ForeignDataWrapper *wrapper;
	List		   *options;
	ListCell	   *lc, *prev;

	/*
	 * Create new relid instance because we use 'private' list as a pointer
	 * list.
	 */
	relid = makeConst(OIDOID,
					  -1,
					  sizeof(Oid),
					  ObjectIdGetDatum(foreigntableid),
					  false, true);

	/* Extract options from FDW objects */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);
	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);

	/*
	 * Split filename option off from the list because filename should be
	 * passed as another parameter to BeginCopyFrom().
	 */
	prev = NULL;
	foreach (lc, options)
	{
		DefElem	   *def = lfirst(lc);
		if (strcmp(def->defname, "filename") == 0)
		{
			filename = makeString(strVal(def->arg));
			options = list_delete_cell(options, lc, prev);
			break;
		}
		prev = lc;
	}
	if (filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
				 errmsg("filename is required for file_fdw scan")));

	/*
	 * Split textarray option off from the list because it's handled
	 * here instead of being passed as another parameter to BeginCopyFrom().
	 */
	prev = NULL;
	foreach (lc, options)
	{
		DefElem	   *def = lfirst(lc);
		if (strcmp(def->defname, "textarray") == 0)
		{
			textarray = defGetBoolean(def);
			options = list_delete_cell(options, lc, prev);
			break;
		}
		prev = lc;
	}
	textarray_param = (Const *) makeBoolConst(textarray,false);

	if (text_array)
	{
		/* make sure the table has one column and it's oy type text[] */
		/* XXX fill in this piece */
	}
	
	/* Construct FdwPlan and store relid and options in private area */
	fplan = makeNode(FdwPlan);
	size = estimate_costs(strVal(filename), rel,
						  &fplan->startup_cost, &fplan->total_cost);
	fplan->explainInfo = generate_explain_info(strVal(filename), size);
	fplan->fdw_private = NIL;
	fplan->fdw_private = lappend(fplan->fdw_private, relid);
	fplan->fdw_private = lappend(fplan->fdw_private, filename);
	fplan->fdw_private = lappend(fplan->fdw_private, textarray_param);
	fplan->fdw_private = lappend(fplan->fdw_private, options);

	return fplan;
}

/*
 * BeginScan()
 *   - initiate access to the file with creating CopyState
 *
 * Parameters for parsing file such as filename and format are passed via
 * generic options of FDW-related objects; foreign-data wrapper, server and
 * foreign table.  User mapping is not used to get options because there is no
 * valid option in context of user mapping.
 */
static FdwExecutionState *
fileBeginScan(FdwPlan *fplan, ParamListInfo params)
{
	Const		   *relid_const;
	Oid				relid;
	Value		   *filename;
    Const          *textarray;
	List		   *options;
	Relation		rel;
	CopyState		cstate;
	FileFdwPrivate *fdw_private;
	FdwExecutionState *festate;

	elog(DEBUG3, "%s called", __FUNCTION__);

	/* Get oid of the relation and option list from private area of FdwPlan. */
	relid_const = list_nth(fplan->fdw_private, 0);
	filename = list_nth(fplan->fdw_private, 1);
	textarray = list_nth(fplan->fdw_private, 2);
	options = list_nth(fplan->fdw_private, 3);

	relid = DatumGetObjectId(relid_const->constvalue);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns.
	 * We open the relation with no lock because it's assumed that appropriate
	 * lock has been acquired already.  The rel should be closed in
	 * fileEndScan().
	 */
	rel = heap_open(relid, NoLock);
	cstate = BeginCopyFrom(rel, strVal(filename), NIL, options);

	/*
	 * Pack file information into private and pass it to subsequent functions.
	 * We also store information enough to call BeginCopyFrom() again.
	 */
	festate = palloc0(sizeof(FdwExecutionState));
	fdw_private = palloc0(sizeof(FileFdwPrivate));
	fdw_private->filename = strVal(filename);
	fdw_private->textarray = textarray->constvalue;
	fdw_private->rel = rel;
	fdw_private->cstate = cstate;
	fdw_private->options = options;
	festate->fdw_private = (void *) fdw_private;

	return festate;
}

/*
 * Iterate()
 *   - create HeapTuple from the record in the file.
 */
static void
fileIterate(FdwExecutionState *festate, TupleTableSlot *slot)
{
	FileFdwPrivate *fdw_private = (FileFdwPrivate *) festate->fdw_private;
	bool			found;
	ErrorContextCallback errcontext;

	elog(DEBUG3, "%s called for \"%s\"", __FUNCTION__, fdw_private->filename);

	/* Set up callback to identify error line number. */
	errcontext.callback = CopyFromErrorCallback;
	errcontext.arg = (void *) fdw_private->cstate;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/*
	 * If next tuple has been found, store it into the slot as materialized
	 * tuple.  Otherwise, clear the slot to tell executor that we have reached
	 * EOF.
	 */
	ExecClearTuple(slot);
	if (fdw_private->textarray)
	{
		char **raw_fields;
		int nfields;
		
		found = NextLineCopyFrom(fdw_private->cstate, &raw_fields, &nfields,
							 NULL);
		if (found)
			makeTextArray(slot, raw_fields, nfields);
	}
	else
	{
		/* let the COPY code do the work */
		found = NextCopyFrom(fdw_private->cstate, slot->tts_values, 
							 slot->tts_isnull, NULL);
	}
	if (found)
		ExecStoreVirtualTuple(slot);

	/*
	 * Cleanup error callback.  We must uninstall callback before leaving
	 * Iterate() because other scan in the same plan tree might generate error.
	 */
	error_context_stack = errcontext.previous;
}

/*
 * Finish scanning foreign table and dispose objects used for this scan.
 */
static void
fileEndScan(FdwExecutionState *festate)
{
	FileFdwPrivate *fdw_private;

	elog(DEBUG3, "%s called", __FUNCTION__);

	fdw_private = (FileFdwPrivate *) festate->fdw_private;
	EndCopyFrom(fdw_private->cstate);

	heap_close(fdw_private->rel, NoLock);
	pfree(fdw_private);
	pfree(festate);
}

/*
 * Execute query with new parameter.
 */
static void
fileReScan(FdwExecutionState *festate)
{
	FileFdwPrivate *fdw_private = (FileFdwPrivate *) festate->fdw_private;

	elog(DEBUG3, "%s called for \"%s\"", __FUNCTION__, fdw_private->filename);

	EndCopyFrom(fdw_private->cstate);
	fdw_private->cstate = BeginCopyFrom(fdw_private->rel,
										fdw_private->filename,
										NIL,
										fdw_private->options);
}

/*
 * Generate explain info string from information about the file.
 */
static char *
generate_explain_info(const char *filename, unsigned long size)
{
	StringInfoData  explainInfo;

	initStringInfo(&explainInfo);

	/*
	 * Construct explain information.
	 */
	appendStringInfo(&explainInfo, "file=\"%s\", size=%lu", filename, size);

	return explainInfo.data;
}

/*
 * Estimate costs of scanning on a foreign table, and return size of the file.
 */
static unsigned long
estimate_costs(const char *filename, RelOptInfo *baserel,
			   double *startup_cost, double *total_cost)
{
	struct stat		stat_buf;
	BlockNumber		pages;
	double			run_cost = 0;
	double			cpu_per_tuple;

	elog(DEBUG3, "%s called", __FUNCTION__);

	/* get size of the file */
	if (stat(filename, &stat_buf) == -1)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));
	}

	/*
	 * The way to estimate costs is almost same as cost_seqscan(), but there
	 * are some differences:
	 * - DISK costs are estimated from file size.
	 * - CPU costs are 10x of seq scan, for overhead of parsing records.
	 */
	pages = stat_buf.st_size / BLCKSZ + (stat_buf.st_size % BLCKSZ > 0 ? 1 : 0);
	run_cost += seq_page_cost * pages;

	/*
	 * file_fdw resets a scan with re-opening the file, so random_page_cost is
	 * added to the startup cost as "heavy disk access".
	 */
	*startup_cost += baserel->baserestrictcost.startup + random_page_cost;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * 10 * baserel->tuples;
	*total_cost = *startup_cost + run_cost;

	return stat_buf.st_size;
}

static void 
makeTextArray(TupleTableSlot *slot, char **raw_fields, int nfields)
{
	
}

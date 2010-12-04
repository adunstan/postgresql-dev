/*-------------------------------------------------------------------------
 *
 * file_fdw.c
 *		  foreign-datga wrapper for server-side flat files.
 *
 * Copyright (c) 2010, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_foreign_server.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "utils/builtins.h"

#include "file_parser.h"

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
	{ "force_not_null",	AttributeRelationId },

	/* Centinel */
	{ NULL,			InvalidOid }
};

/*
 * SQL functions
 */
extern Datum file_fdw_validator(PG_FUNCTION_ARGS);
extern Datum file_fdw_handler(PG_FUNCTION_ARGS);

/*
 * FDW routines
 */
static void fileBeginScan(ForeignScanState *scanstate);
static void fileIterate(ForeignScanState *scanstate);
static void fileClose(ForeignScanState *scanstate);
static void fileReOpen(ForeignScanState *scanstate);
static void fileEstimateCosts(ForeignPath *path, PlannerInfo *root, RelOptInfo *baserel);

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
		else if (strcmp(def->defname, "force_not_null") == 0)
		{
			/* is valid boolean string? */
			defGetBoolean(def);
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
		NULL,				/* ConnectServer */
		NULL,				/* FreeFSConnection */
		fileEstimateCosts,
		NULL,				/* Open */
		fileBeginScan,
		fileIterate,
		fileClose,
		fileReOpen,
	};

	PG_RETURN_POINTER(&file_fdw_routine);
}

/*
 * BeginScan()
 *   - initiate access to the file with creating FileState
 *
 * Parameters for parsing file such as filename and format are passed via
 * generic options of FDW-related objects; foreign-data wrapper, server and
 * foreign table.  User mapping is not used to get options because there is no
 * valid option in context of user mapping.
 */
static void
fileBeginScan(ForeignScanState *scanstate)
{
	ForeignTable   *table;
	ForeignServer  *server;
	ForeignDataWrapper *wrapper;
	List		   *options;
	FileState		fstate;

	elog(DEBUG3, "%s called", __FUNCTION__);

	/* Extract options from FDW objects */
	table = GetForeignTable(scanstate->ss.ss_currentRelation->rd_id);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);
	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);

	/* create FileState and set default settings */
	fstate = FileStateCreate(scanstate->ss.ss_currentRelation, options);

	/* pack file information into reply and pass it to subsequent functions */
	scanstate->reply = (FdwReply *) fstate;
}

/*
 * Iterate()
 *   - create HeapTuple from the record in the file.
 */
static void
fileIterate(ForeignScanState *scanstate)
{
	FileState		fstate = (FileState) scanstate->reply;
	TupleTableSlot *slot = scanstate->ss.ss_ScanTupleSlot;
	HeapTuple		tuple = NULL;

	elog(DEBUG3, "%s called for \"%s\"",
		 __FUNCTION__,
		 FileStateGetFilename(fstate));

	/* get next tuple from the file */
	tuple = FileStateGetNext(fstate, CurrentMemoryContext);

	/*
	 * If next tuple has been found, store it into the slot.  Otherwise,
	 * clear the slot to tell executor that we have reached EOF.
	 */
	if (HeapTupleIsValid(tuple))
	{
		ExecStoreTuple(tuple, slot, InvalidBuffer, true);
	}
	else
		ExecClearTuple(slot);
}

/*
 * Finish scanning foreign table and dispose objects used for this scan.
 */
static void
fileClose(ForeignScanState *scanstate)
{
	FileState		fstate = (FileState) scanstate->reply;

	elog(DEBUG3, "%s called", __FUNCTION__);

	FileStateFree(fstate);
}

/*
 * Execute query with new parameter.
 */
static void
fileReOpen(ForeignScanState *scanstate)
{
	FileState		fstate = (FileState) scanstate->reply;

	elog(DEBUG3, "%s called for \"%s\"",
		 __FUNCTION__,
		 FileStateGetFilename(fstate));

	FileStateReset(fstate);
}

/*
 * Estimate costs of scanning on a foreign table.
 */
static void
fileEstimateCosts(ForeignPath *path, PlannerInfo *root, RelOptInfo *baserel)
{
	RangeTblEntry  *rte;
	ForeignTable   *table;
	int				n;
	const char	  **keywords;
	const char	  **values;
	int				i;
	char		   *filename = NULL;
	struct stat		stat;
	BlockNumber		pages;
	double			run_cost = 0;
	double			startup_cost = 0;
	double			cpu_per_tuple;

	elog(DEBUG3, "%s called", __FUNCTION__);

	/* get filename from generic option of the foreign table */
	rte = planner_rt_fetch(baserel->relid, root);
	table = GetForeignTable(rte->relid);
	keywords = palloc(sizeof(char *) * list_length(table->options));
	values = palloc(sizeof(char *) * list_length(table->options));
	n = flatten_generic_options(table->options, keywords, values);

	for (i = 0; i < n; i++)
	{
		if (strcmp(keywords[i], "filename") == 0)
		{
			filename = pstrdup(values[i]);
			break;
		}
	}

	pfree(keywords);
	pfree(values);

	/* at least filename must be specified */
	if (filename == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
				 errmsg("generic option \"filename\" is required")));
	}

	/* get size of the file */
	if (lstat(filename, &stat) == -1)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));
	}
	pfree(filename);

	/*
	 * The way to estimate costs is almost same as cost_seqscan(), but there
	 * are some differences:
	 * - DISK costs are estimated from file size.
	 * - CPU costs are 2x of seq scan, for overhead of parsing records.
	 */
	pages = stat.st_size / BLCKSZ;
	run_cost += seq_page_cost * pages;

	startup_cost += baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
	cpu_per_tuple *= 2;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = startup_cost + run_cost;
}


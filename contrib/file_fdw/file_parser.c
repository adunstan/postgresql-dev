/*-------------------------------------------------------------------------
 *
 * file_parser.c
 *		Implements the parser of CSV/Text format file.  Heavily based
 *		on src/backend/commands/copy.c .
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
#include "postgres.h"

#include "access/xact.h"
#include "access/reloptions.h"
#include "commands/defrem.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "file_parser.h"

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 *	Represents the end-of-line terminator type of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;

/*
 * This struct contains all the state variables used throughout parsing a file
 * which is in format of CSV or TEXT.
 *
 * Multi-byte encodings: all supported client-side encodings encode multi-byte
 * characters by having the first byte's high bit set. Subsequent bytes of the
 * character can have the high bit not set. When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character, we must
 * use the full pg_encoding_mblen() machinery to skip over multibyte
 * characters, else we might find a false match to a trailing byte. In
 * supported server encodings, there is no possibility of a false match, and
 * it's faster to make useless comparisons to trailing bytes than it is to
 * invoke pg_encoding_mblen() to skip over them. encoding_embeds_ascii is TRUE
 * when we have to do it the hard way.
 */
typedef struct FileStateData
{
	/* low-level state data */
	FILE	   *file;
	EolType		eol_type;		/* EOL type of input */
	int			client_encoding;	/* remote side's character encoding */
	bool		need_transcoding;		/* client encoding diff from server? */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */
	uint64		processed;		/* # of tuples processed */
	bool		done;			/* read all records in the file? */

	/* generic options from the FDW-related objects */
	Relation	rel;			/* foreign table */
	List	   *attnumlist;		/* integer list of attnums to copy */
	char	   *filename;		/* filename, or NULL for STDIN/STDOUT */
	bool		csv_mode;		/* Comma Separated Value format? */
	bool		header_line;	/* CSV header line? */
	char	   *null_print;		/* NULL marker string (server encoding!) */
	int			null_print_len; /* length of same */
	char	   *null_print_client;		/* same converted to client encoding */
	char	   *delim;			/* column delimiter (must be 1 byte) */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
	bool	   *force_notnull_flags;	/* per-column CSV FNN flags */

	/* these are just for error messages, see file_error_callback */
	ErrorContextCallback errcontext;
	const char *cur_relname;	/* table name for error messages */
	int			cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	const char *cur_attval;		/* current att value for error messages */

	/*
	 * These variables are used to reduce overhead in textual parsing.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The FileReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 */
	StringInfoData attribute_buf;

	/*
	 * These variables are used to reduce overhead of memory allocation in the
	 * loop over FileStateGetNext().
	 */
	Datum	   *values;
	bool	   *nulls;
	int			nfields;
	char	  **field_strings;

	/*
	 * The definition of input functions and default expressions are stored
	 * in these variables.
 	 */
	EState	   *estate;
	FmgrInfo   *in_functions;
	Oid		   *typioparams;
	int		   *defmap;
	ExprState **defexprs;		/* array of default att expressions */
	AttrNumber	num_defaults;

	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, convert it
	 * to server encoding there, and then extract the individual attribute
	 * fields into attribute_buf.  line_buf is preserved unmodified so that we
	 * can display it in error messages if appropriate.
	 */
	StringInfoData line_buf;
	bool		line_buf_converted;		/* converted to server encoding? */

	/*
	 * Finally, raw_buf holds raw data read from the data source (file or
	 * client connection).	FileReadLine parses this data sufficiently to
	 * locate line boundaries, then transfers the data to line_buf and
	 * converts it.  Note: we guarantee that there is a \0 at
	 * raw_buf[raw_buf_len].
	 */
#define RAW_BUF_SIZE 65536		/* we palloc RAW_BUF_SIZE+1 bytes */
	char	   *raw_buf;
	int			raw_buf_index;	/* next byte to process */
	int			raw_buf_len;	/* total # of bytes stored */
} FileStateData;


/*
 * These macros centralize code used to process line_buf and raw_buf buffers.
 * They are macros because they often do continue/break control and to avoid
 * function call overhead in tight Iterate() loops.
 *
 * We must use "if (1)" because the usual "do {...} while(0)" wrapper would
 * prevent the continue/break processing from working.	We end the "if (1)"
 * with "else ((void) 0)" to ensure the "if" does not unintentionally match
 * any "else" in the calling code, and to avoid any compiler warnings about
 * empty statements.  See http://www.cit.gu.edu.au/~anthony/info/C/C.macros.
 */

/*
 * This keeps the character read at the top of the loop in the buffer
 * even if there is more than one read-ahead.
 */
#define IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(extralen) \
if (1) \
{ \
	if (raw_buf_ptr + (extralen) >= raw_buf_len && !hit_eof) \
	{ \
		raw_buf_ptr = prev_raw_ptr; /* undo fetch */ \
		need_data = true; \
		continue; \
	} \
} else ((void) 0)

/* This consumes the remainder of the buffer and breaks */
#define IF_NEED_REFILL_AND_EOF_BREAK(extralen) \
if (1) \
{ \
	if (raw_buf_ptr + (extralen) >= raw_buf_len && hit_eof) \
	{ \
		if (extralen) \
			raw_buf_ptr = raw_buf_len; /* consume the partial character */ \
		/* backslash just before EOF, treat as data char */ \
		result = true; \
		break; \
	} \
} else ((void) 0)

/*
 * Transfer any approved data to line_buf; must do this to be sure
 * there is some room in raw_buf.
 */
#define REFILL_LINEBUF \
if (1) \
{ \
	if (raw_buf_ptr > fstate->raw_buf_index) \
	{ \
		appendBinaryStringInfo(&fstate->line_buf, \
							 fstate->raw_buf + fstate->raw_buf_index, \
							   raw_buf_ptr - fstate->raw_buf_index); \
		fstate->raw_buf_index = raw_buf_ptr; \
	} \
} else ((void) 0)

/* Undo any read-ahead and jump out of the block. */
#define NO_END_OF_COPY_GOTO \
if (1) \
{ \
	raw_buf_ptr = prev_raw_ptr + 1; \
	goto not_end_of_copy; \
} else ((void) 0)

/* non-export function prototypes */
static bool FileReadLine(FileState fstate);
static bool FileReadLineText(FileState fstate);
static int FileReadAttributesText(FileState fstate);
static int FileReadAttributesCSV(FileState fstate);
static List *FileGetAttnums(TupleDesc tupDesc, Relation rel,
			   List *attnamelist);
static char *limit_printout_length(const char *str);
static void file_error_callback(void *arg);

/*
 * FileGetData reads data from the source (file or frontend)
 *
 * We attempt to read at least minread, and at most maxread, bytes from
 * the source.	The actual number of bytes read is returned; if this is
 * less than minread, EOF was detected.
 *
 * NB: no data conversion is applied here.
 */
static int
FileGetData(FileState fstate, void *databuf, int minread, int maxread)
{
	int			bytesread = 0;

	bytesread = fread(databuf, 1, maxread, fstate->file);
	if (ferror(fstate->file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						fstate->filename)));

	return bytesread;
}


/*
 * FileLoadRawBuf loads some more data into raw_buf
 *
 * Returns TRUE if able to obtain at least one more byte, else FALSE.
 *
 * If raw_buf_index < raw_buf_len, the unprocessed bytes are transferred
 * down to the start of the buffer and then we load more data after that.
 * This case is used only when a frontend multibyte character crosses a
 * bufferload boundary.
 */
static bool
FileLoadRawBuf(FileState fstate)
{
	int			nbytes;
	int			inbytes;

	if (fstate->raw_buf_index < fstate->raw_buf_len)
	{
		/* Copy down the unprocessed data */
		nbytes = fstate->raw_buf_len - fstate->raw_buf_index;
		memmove(fstate->raw_buf, fstate->raw_buf + fstate->raw_buf_index,
				nbytes);
	}
	else
		nbytes = 0;				/* no data need be saved */

	inbytes = FileGetData(fstate, fstate->raw_buf + nbytes,
						  1, RAW_BUF_SIZE - nbytes);
	nbytes += inbytes;
	fstate->raw_buf[nbytes] = '\0';
	fstate->raw_buf_index = 0;
	fstate->raw_buf_len = nbytes;
	return (inbytes > 0);
}


/*
 * FileStateCreate() makes a FileState for a foreign scan on the relation rel.
 */
FileState
FileStateCreate(Relation rel, List *options)
{
	FileState	fstate;
	bool		format_specified = false;
	ListCell   *option;
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	Form_pg_attribute *attr;
	AttrNumber	attr_count;
	int			attnum;
	ResultRelInfo *resultRelInfo;

	/* Allocate workspace and zero all fields */
	fstate = (FileStateData *) palloc0(sizeof(FileStateData));

	/* Extract information about the file and its format from options */
	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "filename") == 0)
		{
			fstate->filename = defGetString(defel);
		}
		else if (strcmp(defel->defname, "format") == 0)
		{
			char	   *fmt = defGetString(defel);

			if (format_specified)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			format_specified = true;
			if (strcmp(fmt, "text") == 0)
				 /* default format */ ;
			else if (strcmp(fmt, "csv") == 0)
				fstate->csv_mode = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("format \"%s\" not recognized", fmt)));
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (fstate->delim)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			fstate->delim = defGetString(defel);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (fstate->null_print)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			fstate->null_print = defGetString(defel);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (fstate->header_line)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			fstate->header_line = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (fstate->quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			fstate->quote = defGetString(defel);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (fstate->escape)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			fstate->escape = defGetString(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized",
							defel->defname)));
	}

	if (fstate->filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNALBE_TO_CREATE_EXECUTION),
				 errmsg("generic option filename is required")));

	/* Set defaults for omitted options */
	if (!fstate->delim)
		fstate->delim = fstate->csv_mode ? "," : "\t";

	if (!fstate->null_print)
		fstate->null_print = fstate->csv_mode ? "" : "\\N";
	fstate->null_print_len = strlen(fstate->null_print);

	if (fstate->csv_mode)
	{
		if (!fstate->quote)
			fstate->quote = "\"";
		if (!fstate->escape)
			fstate->escape = fstate->quote;
	}

	/*
	 * Disallow unsafe delimiter characters in non-CSV mode.  We can't allow
	 * backslash because it would be ambiguous.  We can't allow the other
	 * cases because data characters matching the delimiter must be
	 * backslashed, and certain backslash combinations are interpreted
	 * non-literally by COPY IN.  Disallowing all lower case ASCII letters is
	 * more than strictly necessary, but seems best for consistency and
	 * future-proofing.  Likewise we disallow all digits though only octal
	 * digits are actually dangerous.
	 */
	if (!fstate->csv_mode &&
		strchr("\\.abcdefghijklmnopqrstuvwxyz0123456789",
			   fstate->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("delimiter cannot be \"%s\"", fstate->delim)));

	/* Check header */
	if (!fstate->csv_mode && fstate->header_line)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("HEADER available only in CSV mode")));

	/* Check quote */
	if (!fstate->csv_mode && fstate->quote != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("quote available only in CSV mode")));

	if (fstate->csv_mode && strlen(fstate->quote) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("quote must be a single one-byte character")));

	if (fstate->csv_mode && fstate->delim[0] == fstate->quote[0])
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("delimiter and quote must be different")));

	/* Check escape */
	if (!fstate->csv_mode && fstate->escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("escape available only in CSV mode")));

	if (fstate->csv_mode && strlen(fstate->escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("escape must be a single one-byte character")));

	/* Don't allow the delimiter to appear in the null string. */
	if (strchr(fstate->null_print, fstate->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("delimiter must not appear in the NULL specification")));

	/* Don't allow the CSV quote char to appear in the null string. */
	if (fstate->csv_mode &&
		strchr(fstate->null_print, fstate->quote[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSV quote character must not appear in the NULL specification")));

	/* Disallow reading server-side files except to superusers. */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to read from a file")));

	fstate->rel = rel;
	tupDesc = RelationGetDescr(fstate->rel);

	/* Generate or convert list of attributes to process */
	fstate->attnumlist = FileGetAttnums(tupDesc, fstate->rel, NIL);

	num_phys_attrs = tupDesc->natts;

	/* Convert force_not_null options in each columns to per-column flags. */
	if (fstate->csv_mode)
	{
		int i;
		Form_pg_attribute *attrs = tupDesc->attrs;

		/* slot for dropped column are necessary */
		fstate->force_notnull_flags =
			(bool *) palloc0(num_phys_attrs * sizeof(bool));

		for (i = 0; i < num_phys_attrs; i++)
		{
			HeapTuple	tuple;
			Datum		datum;
			bool		isnull;
			List	   *options;
			ListCell   *lc;

			if (attrs[i]->attisdropped)
				continue;

			tuple = SearchSysCache2(ATTNUM, rel->rd_id, i + 1);
			if (!HeapTupleIsValid(tuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("attnum \"%d\" of relation \"%s\" does not exist",
								i + 1, RelationGetRelationName(fstate->rel))));

			datum = SysCacheGetAttr(ATTNUM, tuple,
									Anum_pg_attribute_attgenoptions,
									&isnull);
			if (!isnull)
			{
				options = untransformRelOptions(datum);
				foreach (lc, options)
				{
					DefElem	   *def = lfirst(lc);
					if (strcmp(def->defname, "force_not_null") == 0)
					{
						fstate->force_notnull_flags[i] = defGetBoolean(def);
						break;
					}
				}
			}

			ReleaseSysCache(tuple);
		}
	}

	/* Set up variables to avoid per-attribute overhead. */
	initStringInfo(&fstate->attribute_buf);
	initStringInfo(&fstate->line_buf);
	fstate->line_buf_converted = false;
	fstate->raw_buf = (char *) palloc(RAW_BUF_SIZE + 1);
	fstate->raw_buf_index = fstate->raw_buf_len = 0;
	fstate->processed = 0;
	fstate->done = false;

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to validate
	 * data in multibyte encodings.
	 */
	fstate->client_encoding = pg_get_client_encoding();
	fstate->need_transcoding =
		(fstate->client_encoding != GetDatabaseEncoding() ||
		 pg_database_encoding_max_length() > 1);
	/* See Multibyte encoding comment above */
	fstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(fstate->client_encoding);

	fstate->estate = CreateExecutorState(); /* for ExecConstraints() */

	tupDesc = RelationGetDescr(fstate->rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(fstate->attnumlist);
	fstate->num_defaults = 0;

	/* We need a ResultRelInfo to check constraints. */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = fstate->rel;

	fstate->estate->es_result_relations = resultRelInfo;
	fstate->estate->es_num_result_relations = 1;
	fstate->estate->es_result_relation_info = resultRelInfo;

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass to
	 * the input function), and info about defaults and constraints.
	 */
	fstate->in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	fstate->typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	fstate->defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	fstate->defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	for (attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		Oid			in_func_oid;

		/* We don't need info for dropped attributes */
		if (attr[attnum - 1]->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		getTypeInputInfo(attr[attnum - 1]->atttypid,
						 &in_func_oid, &fstate->typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &fstate->in_functions[attnum - 1]);

		/* Get default info if needed */
		if (!list_member_int(fstate->attnumlist, attnum))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node	   *defexpr = build_column_default(fstate->rel, attnum);

			if (defexpr != NULL)
			{
				fstate->defexprs[fstate->num_defaults] =
					ExecPrepareExpr((Expr *) defexpr, fstate->estate);
				fstate->defmap[fstate->num_defaults] = attnum - 1;
				fstate->num_defaults++;
			}
		}
	}

	fstate->values = (Datum *) palloc(num_phys_attrs * sizeof(Datum));
	fstate->nulls = (bool *) palloc(num_phys_attrs * sizeof(bool));

	/* create workspace for FileReadAttributes results */
	fstate->nfields = attr_count;
	fstate->field_strings = (char **) palloc(fstate->nfields * sizeof(char *));

	/* Initialize state variables */
	fstate->eol_type = EOL_UNKNOWN;
	fstate->cur_relname = RelationGetRelationName(fstate->rel);
	fstate->cur_lineno = 0;
	fstate->cur_attname = NULL;
	fstate->cur_attval = NULL;

	/* Set up callback to identify error line number */
	fstate->errcontext.callback = file_error_callback;
	fstate->errcontext.arg = (void *) fstate;
	fstate->errcontext.previous = error_context_stack;
	error_context_stack = &fstate->errcontext;

	return fstate;
}


void
FileStateFree(FileState fstate)
{
	if (fstate == NULL)
		return;

	/* Done, clean up */
	error_context_stack = fstate->errcontext.previous;

	pfree(fstate->values);
	pfree(fstate->nulls);
	pfree(fstate->field_strings);

	pfree(fstate->in_functions);
	pfree(fstate->typioparams);
	pfree(fstate->defmap);
	pfree(fstate->defexprs);

	ExecResetTupleTable(fstate->estate->es_tupleTable, false);

	FreeExecutorState(fstate->estate);

	if (fstate->file != NULL)
		if (FreeFile(fstate->file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m",
							fstate->filename)));

	/* Clean up storage (probably not really necessary) */
	pfree(fstate->attribute_buf.data);
	pfree(fstate->line_buf.data);
	pfree(fstate->raw_buf);
	pfree(fstate);
}


/*
 * error context callback for CSV/Text file parsing
 */
static void
file_error_callback(void *arg)
{
	FileState	fstate = (FileState) arg;

	if (fstate->cur_attname && fstate->cur_attval)
	{
		/* error is relevant to a particular column */
		char	   *attval;

		attval = limit_printout_length(fstate->cur_attval);
		errcontext("scanning %s, line %d, column %s: \"%s\"",
				   fstate->cur_relname, fstate->cur_lineno,
				   fstate->cur_attname, attval);
		pfree(attval);
	}
	else if (fstate->cur_attname)
	{
		/* error is relevant to a particular column, value is NULL */
		errcontext("scanning %s, line %d, column %s: null input",
				   fstate->cur_relname, fstate->cur_lineno,
				   fstate->cur_attname);
	}
	else
	{
		/* error is relevant to a particular line */
		if (fstate->line_buf_converted || !fstate->need_transcoding)
		{
			char	   *lineval;

			lineval = limit_printout_length(fstate->line_buf.data);
			errcontext("scanning %s, line %d: \"%s\"",
					   fstate->cur_relname, fstate->cur_lineno, lineval);
			pfree(lineval);
		}
		else
		{
			/*
			 * Here, the line buffer is still in a foreign encoding, and
			 * indeed it's quite likely that the error is precisely a
			 * failure to do encoding conversion (ie, bad data).  We dare
			 * not try to convert it, and at present there's no way to
			 * regurgitate it without conversion.  So we have to punt and
			 * just report the line number.
			 */
			errcontext("scanning %s, line %d",
					   fstate->cur_relname, fstate->cur_lineno);
		}
	}
}

/*
 * Make sure we don't print an unreasonable amount of data in a message.
 *
 * It would seem a lot easier to just use the sprintf "precision" limit to
 * truncate the string.  However, some versions of glibc have a bug/misfeature
 * that vsnprintf will always fail (return -1) if it is asked to truncate
 * a string that contains invalid byte sequences for the current encoding.
 * So, do our own truncation.  We return a pstrdup'd copy of the input.
 */
static char *
limit_printout_length(const char *str)
{
#define MAX_COPY_DATA_DISPLAY 100

	int			slen = strlen(str);
	int			len;
	char	   *res;

	/* Fast path if definitely okay */
	if (slen <= MAX_COPY_DATA_DISPLAY)
		return pstrdup(str);

	/* Apply encoding-dependent truncation */
	len = pg_mbcliplen(str, slen, MAX_COPY_DATA_DISPLAY);

	/*
	 * Truncate, and add "..." to show we truncated the input.
	 */
	res = (char *) palloc(len + 4);
	memcpy(res, str, len);
	strcpy(res + len, "...");

	return res;
}

/*
 * Read a record from the file and generate HeapTuple.
 */
HeapTuple
FileStateGetNext(FileState fstate, MemoryContext tupleContext)
{
	HeapTuple	tuple = NULL;
	TupleDesc	tupDesc = RelationGetDescr(fstate->rel);
	Form_pg_attribute *attr = tupDesc->attrs;
	AttrNumber	num_phys_attrs = tupDesc->natts;
	int			i;
	ExprContext	*econtext;
	MemoryContext oldcontext = CurrentMemoryContext;
	ListCell   *cur;
	int			fldct;
	int			fieldno;
	char	   *string;

	/* Initialize the state of the file on the first call. */
	if (fstate->file == NULL)
	{
		struct stat st;

		/*
		 * Open the file in read mode and check that the file is a regular file.
		 */
		fstate->file = AllocateFile(fstate->filename, PG_BINARY_R);

		if (fstate->file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m",
							fstate->filename)));

		fstat(fileno(fstate->file), &st);
		if (S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", fstate->filename)));

		/* on input just throw the header line away */
		if (fstate->header_line)
		{
			fstate->cur_lineno++;
			fstate->done = FileReadLine(fstate);
		}
	}

	/* Return NULL once reached EOF. */
	if (fstate->done)
		goto done;

	/*
	 * Generate HeapTuple from each line of CSV/Text file.
	 */
	CHECK_FOR_INTERRUPTS();

	fstate->cur_lineno++;

	/* Reset the per-tuple exprcontext */
	ResetPerTupleExprContext(fstate->estate);

	/* Switch into its memory context */
	MemoryContextSwitchTo(GetPerTupleMemoryContext(fstate->estate));

	/* Initialize all values for row to NULL */
	MemSet(fstate->values, 0, num_phys_attrs * sizeof(Datum));
	MemSet(fstate->nulls, true, num_phys_attrs * sizeof(bool));

	/* Actually read the line into memory here */
	fstate->done = FileReadLine(fstate);

	/*
	 * EOF at start of line means we're done.  If we see EOF after
	 * some characters, we act as though it was newline followed by
	 * EOF, ie, process the line and then exit loop on next iteration.
	 */
	if (fstate->done && fstate->line_buf.len == 0)
		goto done;

	/* Parse the line into de-escaped field values */
	if (fstate->csv_mode)
		fldct = FileReadAttributesCSV(fstate);
	else
		fldct = FileReadAttributesText(fstate);
	fieldno = 0;

	/* Loop to read the user attributes on the line. */
	foreach(cur, fstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		int			m = attnum - 1;

		if (fieldno >= fldct)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("missing data for column \"%s\"",
							NameStr(attr[m]->attname))));
		string = fstate->field_strings[fieldno++];

		if (fstate->csv_mode && string == NULL &&
			fstate->force_notnull_flags[m])
		{
			/* Go ahead and read the NULL string */
			string = fstate->null_print;
		}

		fstate->cur_attname = NameStr(attr[m]->attname);
		fstate->cur_attval = string;
		fstate->values[m] = InputFunctionCall(&fstate->in_functions[m],
											  string,
											  fstate->typioparams[m],
											  attr[m]->atttypmod);
		if (string != NULL)
			fstate->nulls[m] = false;
		fstate->cur_attname = NULL;
		fstate->cur_attval = NULL;
	}

	Assert(fieldno == fstate->nfields);

	/*
	 * Now compute and insert any defaults available for the columns not
	 * provided by the input data.	Anything not processed here or above
	 * will remain NULL.
	 */
	econtext = GetPerTupleExprContext(fstate->estate);
	for (i = 0; i < fstate->num_defaults; i++)
	{
		fstate->values[fstate->defmap[i]] =
			ExecEvalExpr(fstate->defexprs[i], econtext,
						 &fstate->nulls[fstate->defmap[i]], NULL);
	}

	/*
	 * The tuple is allocated in the specific memory context if caller wants.
	*/
	if (tupleContext != NULL)
		MemoryContextSwitchTo(tupleContext);

	/* And now we can form the input tuple. */
	tuple = heap_form_tuple(tupDesc, fstate->values, fstate->nulls);

	/* OK, return the tuple */
	fstate->processed++;

done:
	MemoryContextSwitchTo(oldcontext);

	return tuple;
}


/*
 * Read the next input line and stash it in line_buf, with conversion to
 * server encoding.
 *
 * Result is true if read was terminated by EOF, false if terminated
 * by newline.	The terminating newline or EOF marker is not included
 * in the final value of line_buf.
 */
static bool
FileReadLine(FileState fstate)
{
	bool		result;

	resetStringInfo(&fstate->line_buf);

	/* Mark that encoding conversion hasn't occurred yet */
	fstate->line_buf_converted = false;

	/* Parse data and transfer into line_buf */
	result = FileReadLineText(fstate);

	if (!result)
	{
		/*
		 * If we didn't hit EOF, then we must have transferred the EOL marker
		 * to line_buf along with the data.  Get rid of it.
		 */
		switch (fstate->eol_type)
		{
			case EOL_NL:
				Assert(fstate->line_buf.len >= 1);
				Assert(fstate->line_buf.data[fstate->line_buf.len - 1] == '\n');
				fstate->line_buf.len--;
				fstate->line_buf.data[fstate->line_buf.len] = '\0';
				break;
			case EOL_CR:
				Assert(fstate->line_buf.len >= 1);
				Assert(fstate->line_buf.data[fstate->line_buf.len - 1] == '\r');
				fstate->line_buf.len--;
				fstate->line_buf.data[fstate->line_buf.len] = '\0';
				break;
			case EOL_CRNL:
				Assert(fstate->line_buf.len >= 2);
				Assert(fstate->line_buf.data[fstate->line_buf.len - 2] == '\r');
				Assert(fstate->line_buf.data[fstate->line_buf.len - 1] == '\n');
				fstate->line_buf.len -= 2;
				fstate->line_buf.data[fstate->line_buf.len] = '\0';
				break;
			case EOL_UNKNOWN:
				/* shouldn't get here */
				Assert(false);
				break;
		}
	}

	/* Done reading the line.  Convert it to server encoding. */
	if (fstate->need_transcoding)
	{
		char	   *cvt;

		cvt = pg_client_to_server(fstate->line_buf.data,
								  fstate->line_buf.len);
		if (cvt != fstate->line_buf.data)
		{
			/* transfer converted data back to line_buf */
			resetStringInfo(&fstate->line_buf);
			appendBinaryStringInfo(&fstate->line_buf, cvt, strlen(cvt));
			pfree(cvt);
		}
	}

	/* Now it's safe to use the buffer in error messages */
	fstate->line_buf_converted = true;

	return result;
}

/*
 * FileReadLineText - inner loop of FileReadLine for text mode
 */
static bool
FileReadLineText(FileState fstate)
{
	char	   *raw_buf;
	int			raw_buf_ptr;
	int			raw_buf_len;
	bool		need_data = false;
	bool		hit_eof = false;
	bool		result = false;
	char		mblen_str[2];

	/* CSV variables */
	bool		first_char_in_line = true;
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = '\0';
	char		escapec = '\0';

	if (fstate->csv_mode)
	{
		quotec = fstate->quote[0];
		escapec = fstate->escape[0];
		/* ignore special escape processing if it's the same as quotec */
		if (quotec == escapec)
			escapec = '\0';
	}

	mblen_str[1] = '\0';

	/*
	 * The objective of this loop is to transfer the entire next input line
	 * into line_buf.  Hence, we only care for detecting newlines (\r and/or
	 * \n) and the end-of-copy marker (\.).
	 *
	 * In CSV mode, \r and \n inside a quoted field are just part of the data
	 * value and are put in line_buf.  We keep just enough state to know if we
	 * are currently in a quoted field or not.
	 *
	 * These four characters, and the CSV escape and quote characters, are
	 * assumed the same in frontend and backend encodings.
	 *
	 * For speed, we try to move data from raw_buf to line_buf in chunks
	 * rather than one character at a time.  raw_buf_ptr points to the next
	 * character to examine; any characters from raw_buf_index to raw_buf_ptr
	 * have been determined to be part of the line, but not yet transferred to
	 * line_buf.
	 *
	 * For a little extra speed within the loop, we copy raw_buf and
	 * raw_buf_len into local variables.
	 */
	raw_buf = fstate->raw_buf;
	raw_buf_ptr = fstate->raw_buf_index;
	raw_buf_len = fstate->raw_buf_len;

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/*
		 * Load more data if needed.  Ideally we would just force four bytes
		 * of read-ahead and avoid the many calls to
		 * IF_NEED_REFILL_AND_NOT_EOF_CONTINUE().
		 * One optimization would be to read-ahead four byte here but it
		 * hardly seems worth it, considering the size of the buffer.
		 */
		if (raw_buf_ptr >= raw_buf_len || need_data)
		{
			REFILL_LINEBUF;

			/*
			 * Try to read some more data.	This will certainly reset
			 * raw_buf_index to zero, and raw_buf_ptr must go with it.
			 */
			if (!FileLoadRawBuf(fstate))
				hit_eof = true;
			raw_buf_ptr = 0;
			raw_buf_len = fstate->raw_buf_len;

			/*
			 * If we are completely out of data, break out of the loop,
			 * reporting EOF.
			 */
			if (raw_buf_len <= 0)
			{
				result = true;
				break;
			}
			need_data = false;
		}

		/* OK to fetch a character */
		prev_raw_ptr = raw_buf_ptr;
		c = raw_buf[raw_buf_ptr++];

		if (fstate->csv_mode)
		{
			/*
			 * If character is '\\' or '\r', we may need to look ahead below.
			 * Force fetch of the next character if we don't already have it.
			 * We need to do this before changing CSV state, in case one of
			 * these characters is also the quote or escape character.
			 *
			 * Note: old-protocol does not like forced prefetch, but it's OK
			 * here since we cannot validly be at EOF.
			 */
			if (c == '\\' || c == '\r')
			{
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			}

			/*
			 * Dealing with quotes and escapes here is mildly tricky. If the
			 * quote char is also the escape char, there's no problem - we
			 * just use the char as a toggle. If they are different, we need
			 * to ensure that we only take account of an escape inside a
			 * quoted field and immediately preceding a quote char, and not
			 * the second in a escape-escape sequence.
			 */
			if (in_quote && c == escapec)
				last_was_esc = !last_was_esc;
			if (c == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (c != escapec)
				last_was_esc = false;

			/*
			 * Updating the line count for embedded CR and/or LF chars is
			 * necessarily a little fragile - this test is probably about the
			 * best we can do.	(XXX it's arguable whether we should do this
			 * at all --- is cur_lineno a physical or logical count?)
			 */
			if (in_quote && c == (fstate->eol_type == EOL_NL ? '\n' : '\r'))
				fstate->cur_lineno++;
		}

		/* Process \r */
		if (c == '\r' && (!fstate->csv_mode || !in_quote))
		{
			/* Check for \r\n on first line, _and_ handle \r\n. */
			if (fstate->eol_type == EOL_UNKNOWN ||
				fstate->eol_type == EOL_CRNL)
			{
				/*
				 * If need more data, go back to loop top to load it.
				 *
				 * Note that if we are at EOF, c will wind up as '\0' because
				 * of the guaranteed pad of raw_buf.
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);

				/* get next char */
				c = raw_buf[raw_buf_ptr];

				if (c == '\n')
				{
					raw_buf_ptr++;		/* eat newline */
					fstate->eol_type = EOL_CRNL;		/* in case not set yet */
				}
				else
				{
					/* found \r, but no \n */
					if (fstate->eol_type == EOL_CRNL)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 !fstate->csv_mode ?
							errmsg("literal carriage return found in data") :
							errmsg("unquoted carriage return found in data"),
								 !fstate->csv_mode ?
						errhint("Use \"\\r\" to represent carriage return.") :
								 errhint("Use quoted CSV field to represent carriage return.")));

					/*
					 * if we got here, it is the first line and we didn't find
					 * \n, so don't consume the peeked character
					 */
					fstate->eol_type = EOL_CR;
				}
			}
			else if (fstate->eol_type == EOL_NL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !fstate->csv_mode ?
						 errmsg("literal carriage return found in data") :
						 errmsg("unquoted carriage return found in data"),
						 !fstate->csv_mode ?
					   errhint("Use \"\\r\" to represent carriage return.") :
						 errhint("Use quoted CSV field to represent carriage return.")));
			/* If reach here, we have found the line terminator */
			break;
		}

		/* Process \n */
		if (c == '\n' && (!fstate->csv_mode || !in_quote))
		{
			if (fstate->eol_type == EOL_CR || fstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !fstate->csv_mode ?
						 errmsg("literal newline found in data") :
						 errmsg("unquoted newline found in data"),
						 !fstate->csv_mode ?
						 errhint("Use \"\\n\" to represent newline.") :
					 errhint("Use quoted CSV field to represent newline.")));
			fstate->eol_type = EOL_NL;	/* in case not set yet */
			/* If reach here, we have found the line terminator */
			break;
		}

		/*
		 * In CSV mode, we only recognize \. alone on a line.  This is because
		 * \. is a valid CSV data value.
		 */
		if (c == '\\' && (!fstate->csv_mode || first_char_in_line))
		{
			char		c2;

			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			IF_NEED_REFILL_AND_EOF_BREAK(0);

			/* -----
			 * get next character
			 * Note: we do not change c so if it isn't \., we can fall
			 * through and continue processing for client encoding.
			 * -----
			 */
			c2 = raw_buf[raw_buf_ptr];

			if (c2 == '.')
			{
				raw_buf_ptr++;	/* consume the '.' */

				/*
				 * Note: if we loop back for more data here, it does not
				 * matter that the CSV state change checks are re-executed; we
				 * will come back here with no important state changed.
				 */
				if (fstate->eol_type == EOL_CRNL)
				{
					/* Get the next character */
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					/* if hit_eof, c2 will become '\0' */
					c2 = raw_buf[raw_buf_ptr++];

					if (c2 == '\n')
					{
						if (!fstate->csv_mode)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker does not match previous newline style")));
						else
							NO_END_OF_COPY_GOTO;
					}
					else if (c2 != '\r')
					{
						if (!fstate->csv_mode)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker corrupt")));
						else
							NO_END_OF_COPY_GOTO;
					}
				}

				/* Get the next character */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
				/* if hit_eof, c2 will become '\0' */
				c2 = raw_buf[raw_buf_ptr++];

				if (c2 != '\r' && c2 != '\n')
				{
					if (!fstate->csv_mode)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker corrupt")));
					else
						NO_END_OF_COPY_GOTO;
				}

				if ((fstate->eol_type == EOL_NL && c2 != '\n') ||
					(fstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(fstate->eol_type == EOL_CR && c2 != '\r'))
				{
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));
				}

				/*
				 * Transfer only the data before the \. into line_buf, then
				 * discard the data and the \. sequence.
				 */
				if (prev_raw_ptr > fstate->raw_buf_index)
					appendBinaryStringInfo(&fstate->line_buf,
									 fstate->raw_buf + fstate->raw_buf_index,
									   prev_raw_ptr - fstate->raw_buf_index);
				fstate->raw_buf_index = raw_buf_ptr;
				result = true;	/* report EOF */
				break;
			}
			else if (!fstate->csv_mode)

				/*
				 * If we are here, it means we found a backslash followed by
				 * something other than a period.  In non-CSV mode, anything
				 * after a backslash is special, so we skip over that second
				 * character too.  If we didn't do that \\. would be
				 * considered an eof-of copy, while in non-CSV mode it is a
				 * literal backslash followed by a period.	In CSV mode,
				 * backslashes are not special, so we want to process the
				 * character after the backslash just like a normal character,
				 * so we don't increment in those cases.
				 */
				raw_buf_ptr++;
		}

		/*
		 * This label is for CSV cases where \. appears at the start of a
		 * line, but there is more text after it, meaning it was a data value.
		 * We are more strict for \. in CSV mode because \. could be a data
		 * value, while in non-CSV mode, \. cannot be a data value.
		 */
not_end_of_copy:

		/*
		 * Process all bytes of a multi-byte character as a group.
		 *
		 * We only support multi-byte sequences where the first byte has the
		 * high-bit set, so as an optimization we can avoid this block
		 * entirely if it is not set.
		 */
		if (fstate->encoding_embeds_ascii && IS_HIGHBIT_SET(c))
		{
			int			mblen;

			mblen_str[0] = c;
			/* All our encodings only read the first byte to get the length */
			mblen = pg_encoding_mblen(fstate->client_encoding, mblen_str);
			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(mblen - 1);
			IF_NEED_REFILL_AND_EOF_BREAK(mblen - 1);
			raw_buf_ptr += mblen - 1;
		}
		first_char_in_line = false;
	}							/* end of outer loop */

	/*
	 * Transfer any still-uncopied data to line_buf.
	 */
	REFILL_LINEBUF;

	/* write buffer state back */
	fstate->raw_buf_index = raw_buf_ptr;
	fstate->raw_buf_len = raw_buf_len;

	return result;
}

/*
 *	Return decimal value for a hexadecimal digit
 */
static int
GetDecimalFromHex(char hex)
{
	if (isdigit((unsigned char) hex))
		return hex - '0';
	else
		return tolower((unsigned char) hex) - 'a' + 10;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.
 *
 * The input is in line_buf.  We use attribute_buf to hold the result
 * strings.  fstate->field_strings[k] is set to point to the k'th attribute
 * string, or NULL when the input matches the null marker string.  (Note that
 * the caller cannot check for nulls since the returned string would be the
 * post-de-escaping equivalent, which may look the same as some valid data
 * string.)
 *
 * delim is the column delimiter string (must be just one byte for now).
 * null_print is the null marker string.  Note that this is compared to
 * the pre-de-escaped input string.
 *
 * The return value is the number of fields actually read.	(We error out
 * if this would exceed maxfields, which is the length of
 * fstate->field_strings[].)
 */
static int
FileReadAttributesText(FileState fstate)
{
	char		delimc = fstate->delim[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (fstate->nfields <= 0)
	{
		if (fstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&fstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.	We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into fstate->field_strings[].
	 */
	if (fstate->attribute_buf.maxlen <= fstate->line_buf.len)
		enlargeStringInfo(&fstate->attribute_buf, fstate->line_buf.len);
	output_ptr = fstate->attribute_buf.data;

	/* set pointer variables for loop */
	cur_ptr = fstate->line_buf.data;
	line_end_ptr = fstate->line_buf.data + fstate->line_buf.len;

	/* Outer loop iterates over fields */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;
		bool		saw_non_ascii = false;

		/* Make sure space remains in fstate->field_strings[] */
		if (fieldno >= fstate->nfields)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		fstate->field_strings[fieldno] = output_ptr;

		/* Scan data for field */
		for (;;)
		{
			char		c;

			end_ptr = cur_ptr;
			if (cur_ptr >= line_end_ptr)
				break;
			c = *cur_ptr++;
			if (c == delimc)
			{
				found_delim = true;
				break;
			}
			if (c == '\\')
			{
				if (cur_ptr >= line_end_ptr)
					break;
				c = *cur_ptr++;
				switch (c)
				{
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
						{
							/* handle \013 */
							int			val;

							val = OCTVALUE(c);
							if (cur_ptr < line_end_ptr)
							{
								c = *cur_ptr;
								if (ISOCTAL(c))
								{
									cur_ptr++;
									val = (val << 3) + OCTVALUE(c);
									if (cur_ptr < line_end_ptr)
									{
										c = *cur_ptr;
										if (ISOCTAL(c))
										{
											cur_ptr++;
											val = (val << 3) + OCTVALUE(c);
										}
									}
								}
							}
							c = val & 0377;
							if (c == '\0' || IS_HIGHBIT_SET(c))
								saw_non_ascii = true;
						}
						break;
					case 'x':
						/* Handle \x3F */
						if (cur_ptr < line_end_ptr)
						{
							char		hexchar = *cur_ptr;

							if (isxdigit((unsigned char) hexchar))
							{
								int			val = GetDecimalFromHex(hexchar);

								cur_ptr++;
								if (cur_ptr < line_end_ptr)
								{
									hexchar = *cur_ptr;
									if (isxdigit((unsigned char) hexchar))
									{
										cur_ptr++;
										val = (val << 4) + GetDecimalFromHex(hexchar);
									}
								}
								c = val & 0xff;
								if (c == '\0' || IS_HIGHBIT_SET(c))
									saw_non_ascii = true;
							}
						}
						break;
					case 'b':
						c = '\b';
						break;
					case 'f':
						c = '\f';
						break;
					case 'n':
						c = '\n';
						break;
					case 'r':
						c = '\r';
						break;
					case 't':
						c = '\t';
						break;
					case 'v':
						c = '\v';
						break;

						/*
						 * in all other cases, take the char after '\'
						 * literally
						 */
				}
			}

			/* Add c to output string */
			*output_ptr++ = c;
		}

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		/*
		 * If we de-escaped a non-7-bit-ASCII char, make sure we still have
		 * valid data for the db encoding. Avoid calling strlen here for the
		 * sake of efficiency.
		 */
		if (saw_non_ascii)
		{
			char	   *fld = fstate->field_strings[fieldno];

			pg_verifymbstr(fld, output_ptr - (fld + 1), false);
		}

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (input_len == fstate->null_print_len &&
			strncmp(start_ptr, fstate->null_print, input_len) == 0)
			fstate->field_strings[fieldno] = NULL;

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		if (!found_delim)
			break;
	}

	/* Clean up state of attribute_buf */
	output_ptr--;
	Assert(*output_ptr == '\0');
	fstate->attribute_buf.len = (output_ptr - fstate->attribute_buf.data);

	return fieldno;
}

/*
 * Parse the current line into separate attributes (fields),
 * performing de-escaping as needed.  This has exactly the same API as
 * FileReadAttributesText, except we parse the fields according to
 * "standard" (i.e. common) CSV usage.
 */
static int
FileReadAttributesCSV(FileState fstate)
{
	char		delimc = fstate->delim[0];
	char		quotec = fstate->quote[0];
	char		escapec = fstate->escape[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * We need a special case for zero-column tables: check that the input
	 * line is empty, and return.
	 */
	if (fstate->nfields <= 0)
	{
		if (fstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&fstate->attribute_buf);

	/*
	 * The de-escaped attributes will certainly not be longer than the input
	 * data line, so we can just force attribute_buf to be large enough and
	 * then transfer data without any checks for enough space.	We need to do
	 * it this way because enlarging attribute_buf mid-stream would invalidate
	 * pointers already stored into fstate->field_strings[].
	 */
	if (fstate->attribute_buf.maxlen <= fstate->line_buf.len)
		enlargeStringInfo(&fstate->attribute_buf, fstate->line_buf.len);
	output_ptr = fstate->attribute_buf.data;

	/* set pointer variables for loop */
	cur_ptr = fstate->line_buf.data;
	line_end_ptr = fstate->line_buf.data + fstate->line_buf.len;

	/* Outer loop iterates over fields */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		bool		saw_quote = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;

		/* Make sure space remains in fstate->field_strings[] */
		if (fieldno >= fstate->nfields)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		/* Remember start of field on both input and output sides */
		start_ptr = cur_ptr;
		fstate->field_strings[fieldno] = output_ptr;

		/*
		 * Scan data for field,
		 *
		 * The loop starts in "not quote" mode and then toggles between that
		 * and "in quote" mode. The loop exits normally if it is in "not
		 * quote" mode and a delimiter or line end is seen.
		 */
		for (;;)
		{
			char		c;

			/* Not in quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					goto endfield;
				c = *cur_ptr++;
				/* unquoted field delimiter */
				if (c == delimc)
				{
					found_delim = true;
					goto endfield;
				}
				/* start of quoted field (or part of field) */
				if (c == quotec)
				{
					saw_quote = true;
					break;
				}
				/* Add c to output string */
				*output_ptr++ = c;
			}

			/* In quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("unterminated CSV quoted field")));

				c = *cur_ptr++;

				/* escape within a quoted field */
				if (c == escapec)
				{
					/*
					 * peek at the next char if available, and escape it if it
					 * is an escape char or a quote char
					 */
					if (cur_ptr < line_end_ptr)
					{
						char		nextc = *cur_ptr;

						if (nextc == escapec || nextc == quotec)
						{
							*output_ptr++ = nextc;
							cur_ptr++;
							continue;
						}
					}
				}

				/*
				 * end of quoted field. Must do this test after testing for
				 * escape in case quote char and escape char are the same
				 * (which is the common case).
				 */
				if (c == quotec)
					break;

				/* Add c to output string */
				*output_ptr++ = c;
			}
		}
endfield:

		/* Terminate attribute value in output area */
		*output_ptr++ = '\0';

		/* Check whether raw input matched null marker */
		input_len = end_ptr - start_ptr;
		if (!saw_quote && input_len == fstate->null_print_len &&
			strncmp(start_ptr, fstate->null_print, input_len) == 0)
			fstate->field_strings[fieldno] = NULL;

		fieldno++;
		/* Done if we hit EOL instead of a delim */
		if (!found_delim)
			break;
	}

	/* Clean up state of attribute_buf */
	output_ptr--;
	Assert(*output_ptr == '\0');
	fstate->attribute_buf.len = (output_ptr - fstate->attribute_buf.data);

	return fieldno;
}


/*
 * FileGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 *
 * rel can be NULL ... it's only used for error reports.
 */
static List *
FileGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */
		Form_pg_attribute *attr = tupDesc->attrs;
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			if (attr[i]->attisdropped)
				continue;
			attnums = lappend_int(attnums, i + 1);
		}
	}
	else
	{
		/* Validate the user-supplied list and extract attnums */
		ListCell   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;
			int			i;

			/* Lookup column name */
			attnum = InvalidAttrNumber;
			for (i = 0; i < tupDesc->natts; i++)
			{
				if (tupDesc->attrs[i]->attisdropped)
					continue;
				if (namestrcmp(&(tupDesc->attrs[i]->attname), name) == 0)
				{
					attnum = tupDesc->attrs[i]->attnum;
					break;
				}
			}
			if (attnum == InvalidAttrNumber)
			{
				if (rel != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   name, RelationGetRelationName(rel))));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									name)));
			}
			/* Check for duplicates */
			if (list_member_int(attnums, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								name)));
			attnums = lappend_int(attnums, attnum);
		}
	}

	return attnums;
}

void
FileStateReset(FileState fstate)
{
	/* If the file has not been opened, it is virtually reseted already. */
	if (fstate->file == NULL)
		return;

	if (fseek(fstate->file, 0, SEEK_SET) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek file \"%s\": %m", fstate->filename)));

	/* reset internal state of reading file */
	fstate->processed = 0;
	fstate->done = false;
	fstate->cur_lineno = 0;
	fstate->raw_buf_index = fstate->raw_buf_len = 0;
	resetStringInfo(&fstate->line_buf);
	resetStringInfo(&fstate->attribute_buf);
}

const char *
FileStateGetFilename(FileState fstate)
{
	return fstate->filename;
}

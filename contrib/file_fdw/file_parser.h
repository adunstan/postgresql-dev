/*-------------------------------------------------------------------------
 *
 * file_parser.h
 *	  Implements the parrser of CSV/Text format file.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_PARSER_H
#define FILE_PARSER_H

typedef struct FileStateData *FileState;

/*
 *
 */
FileState FileStateCreate(Relation rel, List *options);
HeapTuple FileStateGetNext(FileState cstate, MemoryContext tupleContext);
void FileStateReset(FileState cstate);
void FileStateFree(FileState cstate);

/*
 * FileState access methods
 */
const char *FileStateGetFilename(FileState cstate);


#endif   /* FILE_PARSER_H */

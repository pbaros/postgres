/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 * Represents the different source/dest cases we need to worry about at
 * the bottom level
 */
typedef enum CopyDest
{
	COPY_FILE,					/* to/from file */
	COPY_OLD_FE,				/* to/from frontend (old protocol) */
	COPY_NEW_FE					/* to/from frontend (new protocol) */
} CopyDest;

/*
 * Represents the type of data returned by CopyReadAttribute()
 */
typedef enum CopyReadResult
{
	NORMAL_ATTR,
	END_OF_LINE,
	END_OF_FILE
} CopyReadResult;

/*
 *	Represents the end-of-line terminator of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;


/* non-export function prototypes */
static void CopyTo(Relation rel, List *attnumlist, bool binary, bool oids,
				   char *delim, char *null_print);
static void CopyFrom(Relation rel, List *attnumlist, bool binary, bool oids,
					 char *delim, char *null_print);
static char *CopyReadAttribute(const char *delim, CopyReadResult *result);
static Datum CopyReadBinaryAttribute(int column_no, FmgrInfo *flinfo,
									 Oid typelem, bool *isnull);
static void CopyAttributeOut(char *string, char *delim);
static List *CopyGetAttnums(Relation rel, List *attnamelist);

static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";

/*
 * Static communication variables ... pretty grotty, but COPY has
 * never been reentrant...
 */
static CopyDest copy_dest;
static FILE *copy_file;			/* if copy_dest == COPY_FILE */
static StringInfo copy_msgbuf;	/* if copy_dest == COPY_NEW_FE */
static bool fe_eof;				/* true if detected end of copy data */
static EolType eol_type;		/* EOL type of input */
static int	copy_lineno;		/* line number for error messages */


/*
 * These static variables are used to avoid incurring overhead for each
 * attribute processed.  attribute_buf is reused on each CopyReadAttribute
 * call to hold the string being read in.  Under normal use it will soon
 * grow to a suitable size, and then we will avoid palloc/pfree overhead
 * for subsequent attributes.  Note that CopyReadAttribute returns a pointer
 * to attribute_buf's data buffer!
 * encoding, if needed, can be set once at the start of the copy operation.
 */
static StringInfoData attribute_buf;

static int	client_encoding;
static int	server_encoding;

/*
 * Internal communications functions
 */
static void SendCopyBegin(bool binary, int natts);
static void ReceiveCopyBegin(bool binary, int natts);
static void SendCopyEnd(bool binary);
static void CopySendData(void *databuf, int datasize);
static void CopySendString(const char *str);
static void CopySendChar(char c);
static void CopySendEndOfRow(bool binary);
static void CopyGetData(void *databuf, int datasize);
static int	CopyGetChar(void);
#define CopyGetEof()  (fe_eof)
static int	CopyPeekChar(void);
static void CopyDonePeek(int c, bool pickup);
static void CopySendInt32(int32 val);
static int32 CopyGetInt32(void);
static void CopySendInt16(int16 val);
static int16 CopyGetInt16(void);

/*
 * Send copy start/stop messages for frontend copies.  These have changed
 * in past protocol redesigns.
 */
static void
SendCopyBegin(bool binary, int natts)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int16	format = (binary ? 1 : 0);
		int		i;

		pq_beginmessage(&buf, 'H');
		pq_sendbyte(&buf, format);			/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);	/* per-column formats */
		pq_endmessage(&buf);
		copy_dest = COPY_NEW_FE;
		copy_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (binary)
			elog(ERROR, "COPY BINARY is not supported to stdout or from stdin");
		pq_putemptymessage('H');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (binary)
			elog(ERROR, "COPY BINARY is not supported to stdout or from stdin");
		pq_putemptymessage('B');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		copy_dest = COPY_OLD_FE;
	}
}

static void
ReceiveCopyBegin(bool binary, int natts)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int16	format = (binary ? 1 : 0);
		int		i;

		pq_beginmessage(&buf, 'G');
		pq_sendbyte(&buf, format);			/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);	/* per-column formats */
		pq_endmessage(&buf);
		copy_dest = COPY_NEW_FE;
		copy_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (binary)
			elog(ERROR, "COPY BINARY is not supported to stdout or from stdin");
		pq_putemptymessage('G');
		copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (binary)
			elog(ERROR, "COPY BINARY is not supported to stdout or from stdin");
		pq_putemptymessage('D');
		copy_dest = COPY_OLD_FE;
	}
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

static void
SendCopyEnd(bool binary)
{
	if (copy_dest == COPY_NEW_FE)
	{
		if (binary)
		{
			/* Need to flush out file trailer word */
			CopySendEndOfRow(true);
		}
		else
		{
			/* Shouldn't have any unsent data */
			Assert(copy_msgbuf->len == 0);
		}
		/* Send Copy Done message */
		pq_putemptymessage('c');
	}
	else
	{
		/* The FE/BE protocol uses \n as newline for all platforms */
		CopySendData("\\.\n", 3);
		pq_endcopyout(false);
	}
}

/*----------
 * CopySendData sends output data to the destination (file or frontend)
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 * CopySendEndOfRow does the appropriate thing at end of each data row
 *
 * NB: no data conversion is applied by these functions
 *----------
 */
static void
CopySendData(void *databuf, int datasize)
{
	switch (copy_dest)
	{
		case COPY_FILE:
			fwrite(databuf, datasize, 1, copy_file);
			if (ferror(copy_file))
				elog(ERROR, "CopySendData: %m");
			break;
		case COPY_OLD_FE:
			if (pq_putbytes((char *) databuf, datasize))
			{
				/* no hope of recovering connection sync, so FATAL */
				elog(FATAL, "CopySendData: connection lost");
			}
			break;
		case COPY_NEW_FE:
			appendBinaryStringInfo(copy_msgbuf, (char *) databuf, datasize);
			break;
	}
}

static void
CopySendString(const char *str)
{
	CopySendData((void *) str, strlen(str));
}

static void
CopySendChar(char c)
{
	CopySendData(&c, 1);
}

static void
CopySendEndOfRow(bool binary)
{
	switch (copy_dest)
	{
		case COPY_FILE:
			if (!binary)
			{
				/* Default line termination depends on platform */
#ifndef WIN32
				CopySendChar('\n');
#else
				CopySendString("\r\n");
#endif
			}
			break;
		case COPY_OLD_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!binary)
				CopySendChar('\n');
			break;
		case COPY_NEW_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!binary)
				CopySendChar('\n');
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', copy_msgbuf->data, copy_msgbuf->len);
			/* Reset copy_msgbuf to empty */
			copy_msgbuf->len = 0;
			copy_msgbuf->data[0] = '\0';
			break;
	}
}

/*
 * CopyGetData reads data from the source (file or frontend)
 * CopyGetChar does the same for single characters
 *
 * CopyGetEof checks if EOF was detected by previous Get operation.
 *
 * Note: when copying from the frontend, we expect a proper EOF mark per
 * protocol; if the frontend simply drops the connection, we raise error.
 * It seems unwise to allow the COPY IN to complete normally in that case.
 *
 * NB: no data conversion is applied by these functions
 */
static void
CopyGetData(void *databuf, int datasize)
{
	switch (copy_dest)
	{
		case COPY_FILE:
			fread(databuf, datasize, 1, copy_file);
			if (feof(copy_file))
				fe_eof = true;
			break;
		case COPY_OLD_FE:
			if (pq_getbytes((char *) databuf, datasize))
			{
				/* Only a \. terminator is legal EOF in old protocol */
				elog(ERROR, "unexpected EOF on client connection");
			}
			break;
		case COPY_NEW_FE:
			while (datasize > 0 && !fe_eof)
			{
				int		avail;

				while (copy_msgbuf->cursor >= copy_msgbuf->len)
				{
					/* Try to receive another message */
					int			mtype;

					mtype = pq_getbyte();
					if (mtype == EOF)
						elog(ERROR, "unexpected EOF on client connection");
					if (pq_getmessage(copy_msgbuf, 0))
						elog(ERROR, "unexpected EOF on client connection");
					switch (mtype)
					{
						case 'd': /* CopyData */
							break;
						case 'c': /* CopyDone */
							/* COPY IN correctly terminated by frontend */
							fe_eof = true;
							return;
						case 'f': /* CopyFail */
							elog(ERROR, "COPY IN failed: %s",
								 pq_getmsgstring(copy_msgbuf));
							break;
						default:
							elog(ERROR, "unexpected message type %c during COPY IN",
								 mtype);
							break;
					}
				}
				avail = copy_msgbuf->len - copy_msgbuf->cursor;
				if (avail > datasize)
					avail = datasize;
				pq_copymsgbytes(copy_msgbuf, databuf, avail);
				databuf = (void *) ((char *) databuf + avail);
				datasize =- avail;
			}
			break;
	}
}

static int
CopyGetChar(void)
{
	int		ch;

	switch (copy_dest)
	{
		case COPY_FILE:
			ch = getc(copy_file);
			break;
		case COPY_OLD_FE:
			ch = pq_getbyte();
			if (ch == EOF)
			{
				/* Only a \. terminator is legal EOF in old protocol */
				elog(ERROR, "unexpected EOF on client connection");
			}
			break;
		case COPY_NEW_FE:
		{
			unsigned char	cc;

			CopyGetData(&cc, 1);
			if (fe_eof)
				ch = EOF;
			else
				ch = cc;
			break;
		}
		default:
			ch = EOF;
			break;
	}
	if (ch == EOF)
		fe_eof = true;
	return ch;
}

/*
 * CopyPeekChar reads a byte in "peekable" mode.
 *
 * after each call to CopyPeekChar, a call to CopyDonePeek _must_
 * follow, unless EOF was returned.
 *
 * CopyDonePeek will either take the peeked char off the stream
 * (if pickup is true) or leave it on the stream (if pickup is false).
 */
static int
CopyPeekChar(void)
{
	int		ch;

	switch (copy_dest)
	{
		case COPY_FILE:
			ch = getc(copy_file);
			break;
		case COPY_OLD_FE:
			ch = pq_peekbyte();
			if (ch == EOF)
			{
				/* Only a \. terminator is legal EOF in old protocol */
				elog(ERROR, "unexpected EOF on client connection");
			}
			break;
		case COPY_NEW_FE:
		{
			unsigned char	cc;

			CopyGetData(&cc, 1);
			if (fe_eof)
				ch = EOF;
			else
				ch = cc;
			break;
		}
		default:
			ch = EOF;
			break;
	}
	if (ch == EOF)
		fe_eof = true;
	return ch;
}

static void
CopyDonePeek(int c, bool pickup)
{
	if (fe_eof)
		return;					/* can't unget an EOF */
	switch (copy_dest)
	{
		case COPY_FILE:
			if (!pickup) 
			{
				/* We don't want to pick it up - so put it back in there */
				ungetc(c, copy_file);
			}
			/* If we wanted to pick it up, it's already done */
			break;
		case COPY_OLD_FE:
			if (pickup)
			{
				/* We want to pick it up */
				(void) pq_getbyte();
			}
			/* If we didn't want to pick it up, just leave it where it sits */
			break;
		case COPY_NEW_FE:
			if (!pickup)
			{
				/* We don't want to pick it up - so put it back in there */
				copy_msgbuf->cursor--;
			}
			/* If we wanted to pick it up, it's already done */
			break;
	}
}


/*
 * These functions do apply some data conversion
 */

/*
 * CopySendInt32 sends an int32 in network byte order
 */
static void
CopySendInt32(int32 val)
{
	uint32		buf;

	buf = htonl((uint32) val);
	CopySendData(&buf, sizeof(buf));
}

/*
 * CopyGetInt32 reads an int32 that appears in network byte order
 */
static int32
CopyGetInt32(void)
{
	uint32		buf;

	CopyGetData(&buf, sizeof(buf));
	return (int32) ntohl(buf);
}

/*
 * CopySendInt16 sends an int16 in network byte order
 */
static void
CopySendInt16(int16 val)
{
	uint16		buf;

	buf = htons((uint16) val);
	CopySendData(&buf, sizeof(buf));
}

/*
 * CopyGetInt16 reads an int16 that appears in network byte order
 */
static int16
CopyGetInt16(void)
{
	uint16		buf;

	CopyGetData(&buf, sizeof(buf));
	return (int16) ntohs(buf);
}


/*
 *	 DoCopy executes the SQL COPY statement.
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = TRUE means we are inserting into the table.)
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.	Otherwise, transfer is between the table and our regular
 * input/output stream. The latter could be either stdin/stdout or a
 * socket, depending on whether we're running under Postmaster control.
 *
 * Iff <binary>, unload or reload in the binary format, as opposed to the
 * more wasteful but more robust and portable text format.
 *
 * Iff <oids>, unload or reload the format that includes OID information.
 * On input, we accept OIDs whether or not the table has an OID column,
 * but silently drop them if it does not.  On output, we report an error
 * if the user asks for OIDs in a table that has none (not providing an
 * OID column might seem friendlier, but could seriously confuse programs).
 *
 * If in the text format, delimit columns with delimiter <delim> and print
 * NULL values as <null_print>.
 *
 * When loading in the text format from an input stream (as opposed to
 * a file), recognize a "." on a line by itself as EOF. Also recognize
 * a stream EOF.  When unloading in the text format to an output stream,
 * write a "." on a line by itself at the end of the data.
 *
 * Do not allow a Postgres user without superuser privilege to read from
 * or write to a file.
 *
 * Do not allow the copy if user doesn't have proper permission to access
 * the table.
 */
void
DoCopy(const CopyStmt *stmt)
{
	RangeVar   *relation = stmt->relation;
	char	   *filename = stmt->filename;
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	List	   *option;
	List	   *attnamelist = stmt->attlist;
	List	   *attnumlist;
	bool		binary = false;
	bool		oids = false;
	char	   *delim = NULL;
	char	   *null_print = NULL;
	Relation	rel;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclResult	aclresult;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		/* XXX: Should we bother checking for doubled options? */

		if (strcmp(defel->defname, "binary") == 0)
		{
			if (binary)
				elog(ERROR, "COPY: BINARY option appears more than once");

			binary = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			if (oids)
				elog(ERROR, "COPY: OIDS option appears more than once");

			oids = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (delim)
				elog(ERROR, "COPY: DELIMITER string may only be defined once in query");

			delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (null_print)
				elog(ERROR, "COPY: NULL representation may only be defined once in query");

			null_print = strVal(defel->arg);
		}
		else
			elog(ERROR, "COPY: option \"%s\" not recognized",
				 defel->defname);
	}

	if (binary && delim)
		elog(ERROR, "You can not specify the DELIMITER in BINARY mode.");

	if (binary && null_print)
		elog(ERROR, "You can not specify NULL in BINARY mode.");

	/* Set defaults */
	if (!delim)
		delim = "\t";

	if (!null_print)
		null_print = "\\N";

	/*
	 * Open and lock the relation, using the appropriate lock type.
	 */
	rel = heap_openrv(relation, (is_from ? RowExclusiveLock : AccessShareLock));

	/* check read-only transaction */
	if (XactReadOnly && !is_from && !isTempNamespace(RelationGetNamespace(rel)))
		elog(ERROR, "transaction is read-only");

	/* Check permissions. */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  required_access);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, RelationGetRelationName(rel));
	if (!pipe && !superuser())
		elog(ERROR, "You must have Postgres superuser privilege to do a COPY "
			 "directly to or from a file.  Anyone can COPY to stdout or "
			 "from stdin.  Psql's \\copy command also works for anyone.");

	/*
	 * Presently, only single-character delimiter strings are supported.
	 */
	if (strlen(delim) != 1)
		elog(ERROR, "COPY delimiter must be a single character");

	/*
	 * Don't allow COPY w/ OIDs to or from a table without them
	 */
	if (oids && !rel->rd_rel->relhasoids)
		elog(ERROR, "COPY: table \"%s\" does not have OIDs",
			 RelationGetRelationName(rel));

	/*
	 * Generate or convert list of attributes to process
	 */
	attnumlist = CopyGetAttnums(rel, attnamelist);

	/*
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&attribute_buf);

	client_encoding = pg_get_client_encoding();
	server_encoding = GetDatabaseEncoding();

	copy_dest = COPY_FILE;		/* default */
	copy_file = NULL;
	copy_msgbuf = NULL;
	fe_eof = false;

	if (is_from)
	{							/* copy from file to database */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				elog(ERROR, "You cannot copy view %s",
					 RelationGetRelationName(rel));
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You cannot change sequence relation %s",
					 RelationGetRelationName(rel));
			else
				elog(ERROR, "You cannot copy object %s",
					 RelationGetRelationName(rel));
		}
		if (pipe)
		{
			if (IsUnderPostmaster)
				ReceiveCopyBegin(binary, length(attnumlist));
			else
				copy_file = stdin;
		}
		else
		{
			struct stat st;

			copy_file = AllocateFile(filename, PG_BINARY_R);

			if (copy_file == NULL)
				elog(ERROR, "COPY command, running in backend with "
					 "effective uid %d, could not open file '%s' for "
					 "reading.  Errno = %s (%d).",
					 (int) geteuid(), filename, strerror(errno), errno);

			fstat(fileno(copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(copy_file);
				elog(ERROR, "COPY: %s is a directory", filename);
			}
		}
		CopyFrom(rel, attnumlist, binary, oids, delim, null_print);
	}
	else
	{							/* copy from database to file */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				elog(ERROR, "You cannot copy view %s",
					 RelationGetRelationName(rel));
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You cannot copy sequence %s",
					 RelationGetRelationName(rel));
			else
				elog(ERROR, "You cannot copy object %s",
					 RelationGetRelationName(rel));
		}
		if (pipe)
		{
			if (IsUnderPostmaster)
				SendCopyBegin(binary, length(attnumlist));
			else
				copy_file = stdout;
		}
		else
		{
			mode_t		oumask; /* Pre-existing umask value */
			struct stat st;

			/*
			 * Prevent write to relative path ... too easy to shoot
			 * oneself in the foot by overwriting a database file ...
			 */
			if (!is_absolute_path(filename))
				elog(ERROR, "Relative path not allowed for server side"
					 " COPY command");

			oumask = umask((mode_t) 022);
			copy_file = AllocateFile(filename, PG_BINARY_W);
			umask(oumask);

			if (copy_file == NULL)
				elog(ERROR, "COPY command, running in backend with "
					 "effective uid %d, could not open file '%s' for "
					 "writing.  Errno = %s (%d).",
					 (int) geteuid(), filename, strerror(errno), errno);
			fstat(fileno(copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(copy_file);
				elog(ERROR, "COPY: %s is a directory", filename);
			}
		}
		CopyTo(rel, attnumlist, binary, oids, delim, null_print);
	}

	if (!pipe)
		FreeFile(copy_file);
	else if (IsUnderPostmaster && !is_from)
		SendCopyEnd(binary);
	pfree(attribute_buf.data);

	/*
	 * Close the relation.	If reading, we can release the AccessShareLock
	 * we got; if writing, we should hold the lock until end of
	 * transaction to ensure that updates will be committed before lock is
	 * released.
	 */
	heap_close(rel, (is_from ? NoLock : AccessShareLock));
}


/*
 * Copy from relation TO file.
 */
static void
CopyTo(Relation rel, List *attnumlist, bool binary, bool oids,
	   char *delim, char *null_print)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	HeapScanDesc scandesc;
	int			num_phys_attrs;
	int			attr_count;
	Form_pg_attribute *attr;
	FmgrInfo   *out_functions;
	Oid		   *elements;
	bool	   *isvarlena;
	char	   *string;
	Snapshot	mySnapshot;
	List	   *cur;
	MemoryContext oldcontext;
	MemoryContext mycontext;

	tupDesc = rel->rd_att;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = length(attnumlist);

	/*
	 * Get info about the columns we need to process.
	 *
	 * +1's here are to avoid palloc(0) in a zero-column table.
	 */
	out_functions = (FmgrInfo *) palloc((num_phys_attrs + 1) * sizeof(FmgrInfo));
	elements = (Oid *) palloc((num_phys_attrs + 1) * sizeof(Oid));
	isvarlena = (bool *) palloc((num_phys_attrs + 1) * sizeof(bool));
	foreach(cur, attnumlist)
	{
		int			attnum = lfirsti(cur);
		Oid			out_func_oid;

		if (binary)
			getTypeBinaryOutputInfo(attr[attnum - 1]->atttypid,
									&out_func_oid, &elements[attnum - 1],
									&isvarlena[attnum - 1]);
		else
			getTypeOutputInfo(attr[attnum - 1]->atttypid,
							  &out_func_oid, &elements[attnum - 1],
							  &isvarlena[attnum - 1]);
		fmgr_info(out_func_oid, &out_functions[attnum - 1]);
	}

	/*
	 * Create a temporary memory context that we can reset once per row
	 * to recover palloc'd memory.  This avoids any problems with leaks
	 * inside datatype output routines, and should be faster than retail
	 * pfree's anyway.  (We don't need a whole econtext as CopyFrom does.)
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "COPY TO",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	if (binary)
	{
		/* Generate header for a binary copy */
		int32		tmp;

		/* Signature */
		CopySendData((char *) BinarySignature, 11);
		/* Flags field */
		tmp = 0;
		if (oids)
			tmp |= (1 << 16);
		CopySendInt32(tmp);
		/* No header extension */
		tmp = 0;
		CopySendInt32(tmp);
	}

	mySnapshot = CopyQuerySnapshot();

	scandesc = heap_beginscan(rel, mySnapshot, 0, NULL);

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		bool		need_delim = false;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(mycontext);
		oldcontext = MemoryContextSwitchTo(mycontext);

		if (binary)
		{
			/* Binary per-tuple header */
			CopySendInt16(attr_count);
			/* Send OID if wanted --- note attr_count doesn't include it */
			if (oids)
			{
				Oid			oid = HeapTupleGetOid(tuple);

				/* Hack --- assume Oid is same size as int32 */
				CopySendInt32(sizeof(int32));
				CopySendInt32(oid);
			}
		}
		else
		{
			/* Text format has no per-tuple header, but send OID if wanted */
			if (oids)
			{
				string = DatumGetCString(DirectFunctionCall1(oidout,
							  ObjectIdGetDatum(HeapTupleGetOid(tuple))));
				CopySendString(string);
				need_delim = true;
			}
		}

		foreach(cur, attnumlist)
		{
			int			attnum = lfirsti(cur);
			Datum		value;
			bool		isnull;

			value = heap_getattr(tuple, attnum, tupDesc, &isnull);

			if (!binary)
			{
				if (need_delim)
					CopySendChar(delim[0]);
				need_delim = true;
			}

			if (isnull)
			{
				if (!binary)
					CopySendString(null_print);		/* null indicator */
				else
					CopySendInt32(-1);				/* null marker */
			}
			else
			{
				if (!binary)
				{
					string = DatumGetCString(FunctionCall3(&out_functions[attnum - 1],
														   value,
								  ObjectIdGetDatum(elements[attnum - 1]),
							Int32GetDatum(attr[attnum - 1]->atttypmod)));
					CopyAttributeOut(string, delim);
				}
				else
				{
					bytea	   *outputbytes;

					outputbytes = DatumGetByteaP(FunctionCall2(&out_functions[attnum - 1],
															   value,
															   ObjectIdGetDatum(elements[attnum - 1])));
					/* We assume the result will not have been toasted */
					CopySendInt32(VARSIZE(outputbytes) - VARHDRSZ);
					CopySendData(VARDATA(outputbytes),
								 VARSIZE(outputbytes) - VARHDRSZ);
				}
			}
		}

		CopySendEndOfRow(binary);

		MemoryContextSwitchTo(oldcontext);
	}

	heap_endscan(scandesc);

	if (binary)
	{
		/* Generate trailer for a binary copy */
		CopySendInt16(-1);
	}

	MemoryContextDelete(mycontext);

	pfree(out_functions);
	pfree(elements);
	pfree(isvarlena);
}


/*
 * error context callback for COPY FROM
 */
static void
copy_in_error_callback(void *arg)
{
	errcontext("COPY FROM, line %d", copy_lineno);
}


/*
 * Copy FROM file to relation.
 */
static void
CopyFrom(Relation rel, List *attnumlist, bool binary, bool oids,
		 char *delim, char *null_print)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	num_phys_attrs,
				attr_count,
				num_defaults;
	FmgrInfo   *in_functions;
	FmgrInfo	oid_in_function;
	Oid		   *elements;
	Oid			oid_in_element;
	ExprState **constraintexprs;
	bool		hasConstraints = false;
	int			i;
	List	   *cur;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	bool		done = false;
	bool		isnull;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	bool		file_has_oids;
	int		   *defmap;
	ExprState **defexprs;		/* array of default att expressions */
	ExprContext *econtext;		/* used for ExecEvalExpr for default atts */
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorContextCallback errcontext;

	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = length(attnumlist);
	num_defaults = 0;

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of
	 * code here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = rel;
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(rel->trigdesc);

	ExecOpenIndices(resultRelInfo);

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	/* Set up a dummy tuple table too */
	tupleTable = ExecCreateTupleTable(1);
	slot = ExecAllocTableSlot(tupleTable);
	ExecSetSlotDescriptor(slot, tupDesc, false);

	econtext = GetPerTupleExprContext(estate);

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass
	 * to the input function), and info about defaults and constraints.
	 * (Which input function we use depends on text/binary format choice.)
	 * +1's here are to avoid palloc(0) in a zero-column table.
	 */
	in_functions = (FmgrInfo *) palloc((num_phys_attrs + 1) * sizeof(FmgrInfo));
	elements = (Oid *) palloc((num_phys_attrs + 1) * sizeof(Oid));
	defmap = (int *) palloc((num_phys_attrs + 1) * sizeof(int));
	defexprs = (ExprState **) palloc((num_phys_attrs + 1) * sizeof(ExprState *));
	constraintexprs = (ExprState **) palloc0((num_phys_attrs + 1) * sizeof(ExprState *));

	for (i = 0; i < num_phys_attrs; i++)
	{
		/* We don't need info for dropped attributes */
		if (attr[i]->attisdropped)
			continue;

		/* Fetch the input function and typelem info */
		if (binary)
			getTypeBinaryInputInfo(attr[i]->atttypid,
								   &in_func_oid, &elements[i]);
		else
			getTypeInputInfo(attr[i]->atttypid,
							 &in_func_oid, &elements[i]);
		fmgr_info(in_func_oid, &in_functions[i]);

		/* Get default info if needed */
		if (!intMember(i + 1, attnumlist))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node   *defexpr = build_column_default(rel, i + 1);

			if (defexpr != NULL)
			{
				defexprs[num_defaults] = ExecPrepareExpr((Expr *) defexpr,
														 estate);
				defmap[num_defaults] = i;
				num_defaults++;
			}
		}

		/* If it's a domain type, set up to check domain constraints */
		if (get_typtype(attr[i]->atttypid) == 'd')
		{
			Param	   *prm;
			Node	   *node;

			/*
			 * Easiest way to do this is to use parse_coerce.c to set up
			 * an expression that checks the constraints.  (At present,
			 * the expression might contain a length-coercion-function call
			 * and/or CoerceToDomain nodes.)  The bottom of the expression
			 * is a Param node so that we can fill in the actual datum during
			 * the data input loop.
			 */
			prm = makeNode(Param);
			prm->paramkind = PARAM_EXEC;
			prm->paramid = 0;
			prm->paramtype = getBaseType(attr[i]->atttypid);

			node = coerce_to_domain((Node *) prm,
									prm->paramtype,
									attr[i]->atttypid,
									COERCE_IMPLICIT_CAST);

			constraintexprs[i] = ExecPrepareExpr((Expr *) node,
												 estate);
			hasConstraints = true;
		}
	}

	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debateable
	 * whether we should do this for COPY, since it's not really an
	 * "INSERT" statement as such. However, executing these triggers
	 * maintains consistency with the EACH ROW triggers that we already
	 * fire on COPY.
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);

	if (!binary)
	{
		file_has_oids = oids;	/* must rely on user to tell us this... */
	}
	else
	{
		/* Read and verify binary header */
		char		readSig[11];
		int32		tmp;

		/* Signature */
		CopyGetData(readSig, 11);
		if (CopyGetEof() || memcmp(readSig, BinarySignature, 11) != 0)
			elog(ERROR, "COPY BINARY: file signature not recognized");
		/* Flags field */
		tmp = CopyGetInt32();
		if (CopyGetEof())
			elog(ERROR, "COPY BINARY: bogus file header (missing flags)");
		file_has_oids = (tmp & (1 << 16)) != 0;
		tmp &= ~(1 << 16);
		if ((tmp >> 16) != 0)
			elog(ERROR, "COPY BINARY: unrecognized critical flags in header");
		/* Header extension length */
		tmp = CopyGetInt32();
		if (CopyGetEof() || tmp < 0)
			elog(ERROR, "COPY BINARY: bogus file header (missing length)");
		/* Skip extension header, if present */
		while (tmp-- > 0)
		{
			CopyGetData(readSig, 1);
			if (CopyGetEof())
				elog(ERROR, "COPY BINARY: bogus file header (wrong length)");
		}
	}

	if (file_has_oids && binary)
	{
		getTypeBinaryInputInfo(OIDOID,
							   &in_func_oid, &oid_in_element);
		fmgr_info(in_func_oid, &oid_in_function);
	}

	values = (Datum *) palloc((num_phys_attrs + 1) * sizeof(Datum));
	nulls = (char *) palloc((num_phys_attrs + 1) * sizeof(char));

	/* Make room for a PARAM_EXEC value for domain constraint checks */
	if (hasConstraints)
		econtext->ecxt_param_exec_vals = (ParamExecData *)
			palloc0(sizeof(ParamExecData));

	/* Initialize static variables */
	fe_eof = false;
	eol_type = EOL_UNKNOWN;
	copy_lineno = 0;

	/* Set up callback to identify error line number */
	errcontext.callback = copy_in_error_callback;
	errcontext.arg = NULL;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	while (!done)
	{
		bool		skip_tuple;
		Oid			loaded_oid = InvalidOid;

		CHECK_FOR_INTERRUPTS();

		copy_lineno++;

		/* Reset the per-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/* Switch into its memory context */
		MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		/* Initialize all values for row to NULL */
		MemSet(values, 0, num_phys_attrs * sizeof(Datum));
		MemSet(nulls, 'n', num_phys_attrs * sizeof(char));

		if (!binary)
		{
			CopyReadResult result = NORMAL_ATTR;
			char	   *string;

			if (file_has_oids)
			{
				string = CopyReadAttribute(delim, &result);

				if (result == END_OF_FILE && *string == '\0')
				{
					/* EOF at start of line: all is well */
					done = true;
					break;
				}

				if (strcmp(string, null_print) == 0)
					elog(ERROR, "NULL Oid");
				else
				{
					loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
											   CStringGetDatum(string)));
					if (loaded_oid == InvalidOid)
						elog(ERROR, "Invalid Oid");
				}
			}

			/*
			 * Loop to read the user attributes on the line.
			 */
			foreach(cur, attnumlist)
			{
				int			attnum = lfirsti(cur);
				int			m = attnum - 1;

				/*
				 * If prior attr on this line was ended by newline or EOF,
				 * complain.
				 */
				if (result != NORMAL_ATTR)
					elog(ERROR, "Missing data for column \"%s\"",
						 NameStr(attr[m]->attname));

				string = CopyReadAttribute(delim, &result);

				if (result == END_OF_FILE && *string == '\0' &&
					cur == attnumlist && !file_has_oids)
				{
					/* EOF at start of line: all is well */
					done = true;
					break;		/* out of per-attr loop */
				}

				if (strcmp(string, null_print) == 0)
				{
					/* we read an SQL NULL, no need to do anything */
				}
				else
				{
					values[m] = FunctionCall3(&in_functions[m],
											  CStringGetDatum(string),
										   ObjectIdGetDatum(elements[m]),
									  Int32GetDatum(attr[m]->atttypmod));
					nulls[m] = ' ';
				}
			}

			if (done)
				break;			/* out of per-row loop */

			/*
			 * Complain if there are more fields on the input line.
			 *
			 * Special case: if we're reading a zero-column table, we
			 * won't yet have called CopyReadAttribute() at all; so do that
			 * and check we have an empty line.  Fortunately we can keep that
			 * silly corner case out of the main line of execution.
			 */
			if (result == NORMAL_ATTR)
			{
				if (attnumlist == NIL && !file_has_oids)
				{
					string = CopyReadAttribute(delim, &result);
					if (result == NORMAL_ATTR || *string != '\0')
						elog(ERROR, "Extra data after last expected column");
					if (result == END_OF_FILE)
					{
						/* EOF at start of line: all is well */
						done = true;
						break;
					}
				}
				else
					elog(ERROR, "Extra data after last expected column");
			}

			/*
			 * If we got some data on the line, but it was ended by EOF,
			 * process the line normally but set flag to exit the loop
			 * when we return to the top.
			 */
			if (result == END_OF_FILE)
				done = true;
		}
		else
		{
			/* binary */
			int16		fld_count;

			fld_count = CopyGetInt16();
			if (CopyGetEof() || fld_count == -1)
			{
				done = true;
				break;
			}

			if (fld_count != attr_count)
				elog(ERROR, "COPY BINARY: tuple field count is %d, expected %d",
					 (int) fld_count, attr_count);

			if (file_has_oids)
			{
				loaded_oid =
					DatumGetObjectId(CopyReadBinaryAttribute(0,
															 &oid_in_function,
															 oid_in_element,
															 &isnull));
				if (isnull || loaded_oid == InvalidOid)
					elog(ERROR, "COPY BINARY: Invalid Oid");
			}

			i = 0;
			foreach(cur, attnumlist)
			{
				int			attnum = lfirsti(cur);
				int			m = attnum - 1;

				i++;
				values[m] = CopyReadBinaryAttribute(i,
													&in_functions[m],
													elements[m],
													&isnull);
				nulls[m] = isnull ? 'n' : ' ';
			}
		}

		/*
		 * Now compute and insert any defaults available for the columns
		 * not provided by the input data.	Anything not processed here or
		 * above will remain NULL.
		 */
		for (i = 0; i < num_defaults; i++)
		{
			values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
											 &isnull, NULL);
			if (!isnull)
				nulls[defmap[i]] = ' ';
		}

		/*
		 * Next apply any domain constraints
		 */
		if (hasConstraints)
		{
			ParamExecData *prmdata = &econtext->ecxt_param_exec_vals[0];

			for (i = 0; i < num_phys_attrs; i++)
			{
				ExprState  *exprstate = constraintexprs[i];

				if (exprstate == NULL)
					continue;	/* no constraint for this attr */

				/* Insert current row's value into the Param value */
				prmdata->value = values[i];
				prmdata->isnull = (nulls[i] == 'n');

				/*
				 * Execute the constraint expression.  Allow the expression
				 * to replace the value (consider e.g. a timestamp precision
				 * restriction).
				 */
				values[i] = ExecEvalExpr(exprstate, econtext,
										 &isnull, NULL);
				nulls[i] = isnull ? 'n' : ' ';
			}
		}

		/*
		 * And now we can form the input tuple.
		 */
		tuple = heap_formtuple(tupDesc, values, nulls);

		if (oids && file_has_oids)
			HeapTupleSetOid(tuple, loaded_oid);

		/*
		 * Triggers and stuff need to be invoked in query context.
		 */
		MemoryContextSwitchTo(oldcontext);

		skip_tuple = false;

		/* BEFORE ROW INSERT Triggers */
		if (resultRelInfo->ri_TrigDesc &&
			resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
		{
			HeapTuple	newtuple;

			newtuple = ExecBRInsertTriggers(estate, resultRelInfo, tuple);

			if (newtuple == NULL)		/* "do nothing" */
				skip_tuple = true;
			else if (newtuple != tuple) /* modified by Trigger(s) */
			{
				heap_freetuple(tuple);
				tuple = newtuple;
			}
		}

		if (!skip_tuple)
		{
			/* Place tuple in tuple slot */
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);

			/*
			 * Check the constraints of the tuple
			 */
			if (rel->rd_att->constr)
				ExecConstraints("CopyFrom", resultRelInfo, slot, estate);

			/*
			 * OK, store the tuple and create index entries for it
			 */
			simple_heap_insert(rel, tuple);

			if (resultRelInfo->ri_NumIndices > 0)
				ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

			/* AFTER ROW INSERT Triggers */
			ExecARInsertTriggers(estate, resultRelInfo, tuple);
		}
	}

	/*
	 * Done, clean up
	 */
	error_context_stack = errcontext.previous;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Execute AFTER STATEMENT insertion triggers
	 */
	ExecASInsertTriggers(estate, resultRelInfo);

	pfree(values);
	pfree(nulls);

	if (!binary)
	{
		pfree(in_functions);
		pfree(elements);
	}

	ExecDropTupleTable(tupleTable, true);

	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);
}


/*
 * Read the value of a single attribute.
 *
 * *result is set to indicate what terminated the read:
 *		NORMAL_ATTR:	column delimiter
 *		END_OF_LINE:	newline
 *		END_OF_FILE:	EOF indicator
 * In all cases, the string read up to the terminator is returned.
 *
 * Note: This function does not care about SQL NULL values -- it
 * is the caller's responsibility to check if the returned string
 * matches what the user specified for the SQL NULL value.
 *
 * delim is the column delimiter string.
 */
static char *
CopyReadAttribute(const char *delim, CopyReadResult *result)
{
	int			c;
	int			delimc = (unsigned char) delim[0];
	int			mblen;
	unsigned char s[2];
	char	   *cvt;
	int			j;

	s[1] = 0;

	/* reset attribute_buf to empty */
	attribute_buf.len = 0;
	attribute_buf.data[0] = '\0';

	/* set default status */
	*result = NORMAL_ATTR;

	for (;;)
	{
		c = CopyGetChar();
		if (c == EOF)
		{
			*result = END_OF_FILE;
			goto copy_eof;
		}
		if (c == '\r')
		{
			if (eol_type == EOL_NL)
				elog(ERROR, "CopyReadAttribute: Literal carriage return data value\n"
							"found in input that has newline termination; use \\r");

			/*	Check for \r\n on first line, _and_ handle \r\n. */
			if (copy_lineno == 1 || eol_type == EOL_CRNL)
			{
				int c2 = CopyPeekChar();
				if (c2 == '\n')
				{
					CopyDonePeek(c2, true);		/* eat newline */
					eol_type = EOL_CRNL;
				}
				else
				{
					/* found \r, but no \n */
					if (eol_type == EOL_CRNL)
						elog(ERROR, "CopyReadAttribute: Literal carriage return data value\n"
									"found in input that has carriage return/newline termination; use \\r");
					/* if we got here, it is the first line and we didn't get \n, so put it back */
					CopyDonePeek(c2, false);
					eol_type = EOL_CR;
				}
			}
			*result = END_OF_LINE;
			break;
		}
		if (c == '\n')
		{
			if (eol_type == EOL_CRNL)
				elog(ERROR, "CopyReadAttribute: Literal newline data value found in input\n"
							"that has carriage return/newline termination; use \\n");
			if (eol_type == EOL_CR)
				elog(ERROR, "CopyReadAttribute: Literal newline data value found in input\n"
							"that has carriage return termination; use \\n");
			eol_type = EOL_NL;
			*result = END_OF_LINE;
			break;
		}
		if (c == delimc)
			break;
		if (c == '\\')
		{
			c = CopyGetChar();
			if (c == EOF)
			{
				*result = END_OF_FILE;
				goto copy_eof;
			}
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
						int			val;

						val = OCTVALUE(c);
						c = CopyPeekChar();
						if (ISOCTAL(c))
						{
							val = (val << 3) + OCTVALUE(c);
							CopyDonePeek(c, true /* pick up */ );
							c = CopyPeekChar();
							if (ISOCTAL(c))
							{
								val = (val << 3) + OCTVALUE(c);
								CopyDonePeek(c, true /* pick up */ );
							}
							else
							{
								if (c == EOF)
								{
									*result = END_OF_FILE;
									goto copy_eof;
								}
								CopyDonePeek(c, false /* put back */ );
							}
						}
						else
						{
							if (c == EOF)
							{
								*result = END_OF_FILE;
								goto copy_eof;
							}
							CopyDonePeek(c, false /* put back */ );
						}
						c = val & 0377;
					}
					break;

					/*
					 * This is a special hack to parse `\N' as
					 * <backslash-N> rather then just 'N' to provide
					 * compatibility with the default NULL output. -- pe
					 */
				case 'N':
					appendStringInfoCharMacro(&attribute_buf, '\\');
					c = 'N';
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
				case '.':
					if (eol_type == EOL_CRNL)
					{
						c = CopyGetChar();
						if (c == '\n')
							elog(ERROR, "CopyReadAttribute: end-of-copy termination does not match previous input");
						if (c != '\r')
							elog(ERROR, "CopyReadAttribute: end-of-copy marker corrupt");
					}
					c = CopyGetChar();
					if (c != '\r' && c != '\n')
						elog(ERROR, "CopyReadAttribute: end-of-copy marker corrupt");
					if (((eol_type == EOL_NL || eol_type == EOL_CRNL) && c != '\n') ||
					    (eol_type == EOL_CR && c != '\r'))
						elog(ERROR, "CopyReadAttribute: end-of-copy termination does not match previous input");
					/*
					 * In protocol version 3, we should ignore anything after
					 * \. up to the protocol end of copy data.  (XXX maybe
					 * better not to treat \. as special?)
					 */
					if (copy_dest == COPY_NEW_FE)
					{
						while (c != EOF)
						{
							c = CopyGetChar();
						}
					}
					*result = END_OF_FILE;
					goto copy_eof;
			}
		}
		appendStringInfoCharMacro(&attribute_buf, c);

		/* XXX shouldn't this be done even when encoding is the same? */
		if (client_encoding != server_encoding)
		{
			/* get additional bytes of the char, if any */
			s[0] = c;
			mblen = pg_encoding_mblen(client_encoding, s);
			for (j = 1; j < mblen; j++)
			{
				c = CopyGetChar();
				if (c == EOF)
				{
					*result = END_OF_FILE;
					goto copy_eof;
				}
				appendStringInfoCharMacro(&attribute_buf, c);
			}
		}
	}

copy_eof:

	if (client_encoding != server_encoding)
	{
		cvt = (char *) pg_client_to_server((unsigned char *) attribute_buf.data,
										   attribute_buf.len);
		if (cvt != attribute_buf.data)
		{
			/* transfer converted data back to attribute_buf */
			attribute_buf.len = 0;
			attribute_buf.data[0] = '\0';
			appendBinaryStringInfo(&attribute_buf, cvt, strlen(cvt));
		}
	}

	return attribute_buf.data;
}

/*
 * Read a binary attribute
 */
static Datum
CopyReadBinaryAttribute(int column_no, FmgrInfo *flinfo, Oid typelem,
						bool *isnull)
{
	int32		fld_size;
	Datum		result;

	fld_size = CopyGetInt32();
	if (CopyGetEof())
		elog(ERROR, "COPY BINARY: unexpected EOF");
	if (fld_size == -1)
	{
		*isnull = true;
		return (Datum) 0;
	}
	if (fld_size < 0)
		elog(ERROR, "COPY BINARY: bogus size for field %d", column_no);

	/* reset attribute_buf to empty, and load raw data in it */
	attribute_buf.len = 0;
	attribute_buf.data[0] = '\0';
	attribute_buf.cursor = 0;

	enlargeStringInfo(&attribute_buf, fld_size);

	CopyGetData(attribute_buf.data, fld_size);
	if (CopyGetEof())
		elog(ERROR, "COPY BINARY: unexpected EOF");

	attribute_buf.len = fld_size;
	attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	result = FunctionCall2(flinfo,
						   PointerGetDatum(&attribute_buf),
						   ObjectIdGetDatum(typelem));

	/* Trouble if it didn't eat the whole buffer */
	if (attribute_buf.cursor != attribute_buf.len)
		elog(ERROR, "Improper binary format in field %d", column_no);

	*isnull = false;
	return result;
}

/*
 * Send text representation of one attribute, with conversion and escaping
 */
static void
CopyAttributeOut(char *server_string, char *delim)
{
	char	   *string;
	char		c;
	char		delimc = delim[0];
	bool		same_encoding;
	int			mblen;
	int			i;

	same_encoding = (server_encoding == client_encoding);
	if (!same_encoding)
		string = (char *) pg_server_to_client((unsigned char *) server_string,
											  strlen(server_string));
	else
		string = server_string;

	for (; (c = *string) != '\0'; string += mblen)
	{
		mblen = 1;

		switch (c)
		{
			case '\b':
				CopySendString("\\b");
				break;
			case '\f':
				CopySendString("\\f");
				break;
			case '\n':
				CopySendString("\\n");
				break;
			case '\r':
				CopySendString("\\r");
				break;
			case '\t':
				CopySendString("\\t");
				break;
			case '\v':
				CopySendString("\\v");
				break;
			case '\\':
				CopySendString("\\\\");
				break;
			default:
				if (c == delimc)
					CopySendChar('\\');
				CopySendChar(c);

				/*
				 * We can skip pg_encoding_mblen() overhead when encoding
				 * is same, because in valid backend encodings, extra
				 * bytes of a multibyte character never look like ASCII.
				 */
				if (!same_encoding)
				{
					/* send additional bytes of the char, if any */
					mblen = pg_encoding_mblen(client_encoding, string);
					for (i = 1; i < mblen; i++)
						CopySendChar(string[i]);
				}
				break;
		}
	}
}

/*
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 */
static List *
CopyGetAttnums(Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */
		TupleDesc	tupDesc = RelationGetDescr(rel);
		Form_pg_attribute *attr = tupDesc->attrs;
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			if (attr[i]->attisdropped)
				continue;
			attnums = lappendi(attnums, i + 1);
		}
	}
	else
	{
		/* Validate the user-supplied list and extract attnums */
		List	   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;

			/* Lookup column name, elog on failure */
			/* Note we disallow system columns here */
			attnum = attnameAttNum(rel, name, false);
			/* Check for duplicates */
			if (intMember(attnum, attnums))
				elog(ERROR, "Attribute \"%s\" specified more than once", name);
			attnums = lappendi(attnums, attnum);
		}
	}

	return attnums;
}

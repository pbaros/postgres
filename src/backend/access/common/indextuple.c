/*-------------------------------------------------------------------------
 *
 * indextuple.c--
 *	   This file contains index tuple accessor and mutator routines,
 *	   as well as a few various tuple utilities.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/heapam.h>
#include <access/ibit.h>
#include <access/itup.h>
#include <access/tupmacs.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/* ----------------------------------------------------------------
 *				  index_ tuple interface routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		index_formtuple
 * ----------------
 */
IndexTuple
index_formtuple(TupleDesc tupleDescriptor,
				Datum value[],
				char null[])
{
	register char *tp;			/* tuple pointer */
	IndexTuple	tuple;			/* return tuple */
	Size		size,
				hoff;
	int			i;
	unsigned short infomask = 0;
	bool		hasnull = false;
	uint16		tupmask = 0;
	int			numberOfAttributes = tupleDescriptor->natts;

	if (numberOfAttributes > MaxIndexAttributeNumber)
		elog(ERROR, "index_formtuple: numberOfAttributes of %d > %d",
			 numberOfAttributes, MaxIndexAttributeNumber);


	for (i = 0; i < numberOfAttributes && !hasnull; i++)
	{
		if (null[i] != ' ')
			hasnull = true;
	}

	if (hasnull)
		infomask |= INDEX_NULL_MASK;

	hoff = IndexInfoFindDataOffset(infomask);
	size = hoff
		+ ComputeDataSize(tupleDescriptor,
						  value, null);
	size = DOUBLEALIGN(size);	/* be conservative */

	tp = (char *) palloc(size);
	tuple = (IndexTuple) tp;
	MemSet(tp, 0, (int) size);

	DataFill((char *) tp + hoff,
			 tupleDescriptor,
			 value,
			 null,
			 &tupmask,
			 (hasnull ? (bits8 *) tp + sizeof(*tuple) : NULL));

	/*
	 * We do this because DataFill wants to initialize a "tupmask" which
	 * is used for HeapTuples, but we want an indextuple infomask.	The
	 * only "relevent" info is the "has variable attributes" field, which
	 * is in mask position 0x02.  We have already set the null mask above.
	 */

	if (tupmask & 0x02)
		infomask |= INDEX_VAR_MASK;

	/*
	 * Here we make sure that we can actually hold the size.  We also want
	 * to make sure that size is not aligned oddly.  This actually is a
	 * rather odd way to make sure the size is not too large overall.
	 */

	if (size & 0xE000)
		elog(ERROR, "index_formtuple: data takes %d bytes: too big", size);


	infomask |= size;

	/* ----------------
	 * initialize metadata
	 * ----------------
	 */
	tuple->t_info = infomask;
	return (tuple);
}

/* ----------------
 *		nocache_index_getattr
 *
 *		This gets called from index_getattr() macro, and only in cases
 *		where we can't use cacheoffset and the value is not null.
 *
 *		This caches attribute offsets in the attribute descriptor.
 *
 *		an alternate way to speed things up would be to cache offsets
 *		with the tuple, but that seems more difficult unless you take
 *		the storage hit of actually putting those offsets into the
 *		tuple you send to disk.  Yuck.
 *
 *		This scheme will be slightly slower than that, but should
 *		preform well for queries which hit large #'s of tuples.  After
 *		you cache the offsets once, examining all the other tuples using
 *		the same attribute descriptor will go much quicker. -cim 5/4/91
 * ----------------
 */
Datum
nocache_index_getattr(IndexTuple tup,
			 int attnum,
			 TupleDesc tupleDesc,
			 bool *isnull)
{
	register char *tp;			/* ptr to att in tuple */
	register char *bp = NULL;	/* ptr to att in tuple */
	int			slow;			/* do we have to walk nulls? */
	register int data_off;		/* tuple data offset */
	AttributeTupleForm *att = tupleDesc->attrs;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */

	/* ----------------
	 *	 Three cases:
	 *
	 *	 1: No nulls and no variable length attributes.
	 *	 2: Has a null or a varlena AFTER att.
	 *	 3: Has nulls or varlenas BEFORE att.
	 * ----------------
	 */

#ifdef IN_MACRO
/* This is handled in the macro */
	Assert(PointerIsValid(isnull));
	Assert(attnum > 0);

	*isnull = false;
#endif

	data_off = IndexTupleHasMinHeader(tup) ? sizeof *tup :
		IndexInfoFindDataOffset(tup->t_info);

	if (IndexTupleNoNulls(tup))
	{
		attnum--;

#ifdef IN_MACRO
/* This is handled in the macro */
	
		/* first attribute is always at position zero */

		if (attnum == 1)
		{
			return (Datum) fetchatt(&(att[0]), (char *) tup + data_off);
		}
		if (att[attnum]->attcacheoff > 0)
		{
			return (Datum) fetchatt(&(att[attnum]),
							 (char *) tup + data_off +
							 att[attnum]->attcacheoff);
		}
#endif

		tp = (char *) tup + data_off;

		slow = 0;
	}
	else
	{							/* there's a null somewhere in the tuple */

		slow = 0;
		/* ----------------
		 *		check to see if desired att is null
		 * ----------------
		 */

		attnum--;

		bp = (char *) tup + sizeof(*tup);		/* "knows" t_bits are
												 * here! */
#ifdef IN_MACRO
/* This is handled in the macro */
		
		if (att_isnull(attnum, bp))
		{
			*isnull = true;
			return (Datum)NULL;
		}
#endif

		/* ----------------
		 *		Now check to see if any preceeding bits are null...
		 * ----------------
		 */
		{
			register int i = 0; /* current offset in bp */
			register int mask;	/* bit in byte we're looking at */
			register char n;	/* current byte in bp */
			register int byte,
						finalbit;

			byte = attnum >> 3;
			finalbit = attnum & 0x07;

			for (; i <= byte; i++)
			{
				n = bp[i];
				if (i < byte)
				{
					/* check for nulls in any "earlier" bytes */
					if ((~n) != 0)
					{
						slow++;
						break;
					}
				}
				else
				{
					/* check for nulls "before" final bit of last byte */
					mask = (finalbit << 1) - 1;
					if ((~n) & mask)
						slow++;
				}
			}
		}
		tp = (char *) tup + data_off;
	}

	/* now check for any non-fixed length attrs before our attribute */

	if (!slow)
	{
		if (att[attnum]->attcacheoff > 0)
		{
			return (Datum) fetchatt(&(att[attnum]),
							 tp + att[attnum]->attcacheoff);
		}
		else if (!IndexTupleAllFixed(tup))
		{
			register int j = 0;

			for (j = 0; j < attnum && !slow; j++)
				if (att[j]->attlen < 1)
					slow = 1;
		}
	}

	/*
	 * if slow is zero, and we got here, we know that we have a tuple with
	 * no nulls.  We also know that we have to initialize the remainder of
	 * the attribute cached offset values.
	 */

	if (!slow)
	{
		register int j = 1;
		register long off;

		/*
		 * need to set cache for some atts
		 */

		att[0]->attcacheoff = 0;

		while (att[j]->attcacheoff > 0)
			j++;

		off = att[j - 1]->attcacheoff +
			att[j - 1]->attlen;

		for (; j < attnum + 1; j++)
		{

			/*
			 * Fix me when going to a machine with more than a four-byte
			 * word!
			 */

			switch (att[j]->attlen)
			{
				case -1:
					off = (att[j]->attalign == 'd') ?
						DOUBLEALIGN(off) : INTALIGN(off);
					break;
				case sizeof(char):
					break;
				case sizeof(short):
					off = SHORTALIGN(off);
					break;
				case sizeof(int32):
					off = INTALIGN(off);
					break;
				default:
					if (att[j]->attlen > sizeof(int32))
						off = (att[j]->attalign == 'd') ?
							DOUBLEALIGN(off) : LONGALIGN(off);
					else
						elog(ERROR, "nocache_index_getattr: attribute %d has len %d",
							 j, att[j]->attlen);
					break;

			}

			att[j]->attcacheoff = off;
			off += att[j]->attlen;
		}

		return (Datum) fetchatt(&(att[attnum]),
						 tp + att[attnum]->attcacheoff);
	}
	else
	{
		register bool usecache = true;
		register int off = 0;
		register int i;

		/*
		 * Now we know that we have to walk the tuple CAREFULLY.
		 */

		for (i = 0; i < attnum; i++)
		{
			if (!IndexTupleNoNulls(tup))
			{
				if (att_isnull(i, bp))
				{
					usecache = false;
					continue;
				}
			}

			if (usecache && att[i]->attcacheoff > 0)
			{
				off = att[i]->attcacheoff;
				if (att[i]->attlen == -1)
					usecache = false;
				else
					continue;
			}

			if (usecache)
				att[i]->attcacheoff = off;
			switch (att[i]->attlen)
			{
				case sizeof(char):
					off++;
					break;
				case sizeof(short):
					off = SHORTALIGN(off) +sizeof(short);
					break;
				case sizeof(int32):
					off = INTALIGN(off) +sizeof(int32);
					break;
				case -1:
					usecache = false;
					off = (att[i]->attalign == 'd') ?
						DOUBLEALIGN(off) : INTALIGN(off);
					off += VARSIZE(tp + off);
					break;
				default:
					if (att[i]->attlen > sizeof(int32))
						off = (att[i]->attalign == 'd') ?
							DOUBLEALIGN(off) + att[i]->attlen :
							LONGALIGN(off) + att[i]->attlen;
					else
						elog(ERROR, "nocache_index_getattr2: attribute %d has len %d",
							 i, att[i]->attlen);

					break;
			}
		}

		/*
		 * I don't know why this code was missed here! I've got it from
		 * heaptuple.c:nocachegetattr(). - vadim 06/12/97
		 */
		switch (att[attnum]->attlen)
		{
			case -1:
				off = (att[attnum]->attalign == 'd') ?
					DOUBLEALIGN(off) : INTALIGN(off);
				break;
			case sizeof(char):
				break;
			case sizeof(short):
				off = SHORTALIGN(off);
				break;
			case sizeof(int32):
				off = INTALIGN(off);
				break;
			default:
				if (att[attnum]->attlen < sizeof(int32))
					elog(ERROR, "nocache_index_getattr: attribute %d has len %d",
						 attnum, att[attnum]->attlen);
				if (att[attnum]->attalign == 'd')
					off = DOUBLEALIGN(off);
				else
					off = LONGALIGN(off);
				break;
		}

		return (Datum) fetchatt(&att[attnum], tp + off);
	}
}

RetrieveIndexResult
FormRetrieveIndexResult(ItemPointer indexItemPointer,
						ItemPointer heapItemPointer)
{
	RetrieveIndexResult result;

	Assert(ItemPointerIsValid(indexItemPointer));
	Assert(ItemPointerIsValid(heapItemPointer));

	result = (RetrieveIndexResult) palloc(sizeof *result);

	result->index_iptr = *indexItemPointer;
	result->heap_iptr = *heapItemPointer;

	return (result);
}

/*
 * Copies source into target.  If *target == NULL, we palloc space; otherwise
 * we assume we have space that is already palloc'ed.
 */
void
CopyIndexTuple(IndexTuple source, IndexTuple *target)
{
	Size		size;
	IndexTuple	ret;

	size = IndexTupleSize(source);
	if (*target == NULL)
	{
		*target = (IndexTuple) palloc(size);
	}

	ret = *target;
	memmove((char *) ret, (char *) source, size);
}

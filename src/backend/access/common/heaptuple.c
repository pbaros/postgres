 /*-------------------------------------------------------------------------
 *
 * heaptuple.c--
 *	  This file contains heap tuple accessor and mutator routines, as well
 *	  as a few various tuple utilities.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 * NOTES
 *	  The old interface functions have been converted to macros
 *	  and moved to heapam.h
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/heapam.h>
#include <access/htup.h>
#include <access/transam.h>
#include <access/tupmacs.h>
#include <storage/bufpage.h>
#include <utils/memutils.h>

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/* Used by heap_getattr() macro, for speed */
long heap_sysoffset[] = {
/* Only the first one is pass-by-ref, and is handled specially in the macro */
		offsetof(HeapTupleData, t_ctid),		
		offsetof(HeapTupleData, t_oid),
		offsetof(HeapTupleData, t_xmin),
		offsetof(HeapTupleData, t_cmin),
		offsetof(HeapTupleData, t_xmax),
		offsetof(HeapTupleData, t_cmax)
};

/* this is so the sparcstation debugger works */

#if !defined(NO_ASSERT_CHECKING) && defined(sparc) && defined(sunos4)
#define register
#endif							/* !NO_ASSERT_CHECKING && sparc && sunos4 */

/* ----------------------------------------------------------------
 *						misc support routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ComputeDataSize
 * ----------------
 */
Size
ComputeDataSize(TupleDesc tupleDesc,
				Datum value[],
				char nulls[])
{
	uint32		data_length;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	AttributeTupleForm *att = tupleDesc->attrs;

	for (data_length = 0, i = 0; i < numberOfAttributes; i++)
	{
		if (nulls[i] != ' ')
			continue;

		switch (att[i]->attlen)
		{
			case -1:

				/*
				 * This is the size of the disk representation and so must
				 * include the additional sizeof long.
				 */
				if (att[i]->attalign == 'd')
				{
					data_length = DOUBLEALIGN(data_length)
						+ VARSIZE(DatumGetPointer(value[i]));
				}
				else
				{
					data_length = INTALIGN(data_length)
						+ VARSIZE(DatumGetPointer(value[i]));
				}
				break;
			case sizeof(char):
				data_length++;
				break;
			case sizeof(short):
				data_length = SHORTALIGN(data_length + sizeof(short));
				break;
			case sizeof(int32):
				data_length = INTALIGN(data_length + sizeof(int32));
				break;
			default:
				if (att[i]->attlen < sizeof(int32))
					elog(ERROR, "ComputeDataSize: attribute %d has len %d",
						 i, att[i]->attlen);
				if (att[i]->attalign == 'd')
					data_length = DOUBLEALIGN(data_length) + att[i]->attlen;
				else
					data_length = LONGALIGN(data_length) + att[i]->attlen;
				break;
		}
	}

	return data_length;
}

/* ----------------
 *		DataFill
 * ----------------
 */
void
DataFill(char *data,
		 TupleDesc tupleDesc,
		 Datum value[],
		 char nulls[],
		 uint16 *infomask,
		 bits8 *bit)
{
	bits8	   *bitP = 0;
	int			bitmask = 0;
	uint32		data_length;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	AttributeTupleForm *att = tupleDesc->attrs;

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = CSIGNBIT;
	}

	*infomask = 0;

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (bit != NULL)
		{
			if (bitmask != CSIGNBIT)
			{
				bitmask <<= 1;
			}
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (nulls[i] == 'n')
			{
				*infomask |= HEAP_HASNULL;
				continue;
			}

			*bitP |= bitmask;
		}

		switch (att[i]->attlen)
		{
			case -1:
				*infomask |= HEAP_HASVARLENA;
				if (att[i]->attalign == 'd')
				{
					data = (char *) DOUBLEALIGN(data);
				}
				else
				{
					data = (char *) INTALIGN(data);
				}
				data_length = VARSIZE(DatumGetPointer(value[i]));
				memmove(data, DatumGetPointer(value[i]), data_length);
				data += data_length;
				break;
			case sizeof(char):
				*data = att[i]->attbyval ?
					DatumGetChar(value[i]) : *((char *) value[i]);
				data += sizeof(char);
				break;
			case sizeof(int16):
				data = (char *) SHORTALIGN(data);
				*(short *) data = (att[i]->attbyval ?
								   DatumGetInt16(value[i]) :
								   *((short *) value[i]));
				data += sizeof(short);
				break;
			case sizeof(int32):
				data = (char *) INTALIGN(data);
				*(int32 *) data = (att[i]->attbyval ?
								   DatumGetInt32(value[i]) :
								   *((int32 *) value[i]));
				data += sizeof(int32);
				break;
			default:
				if (att[i]->attlen < sizeof(int32))
					elog(ERROR, "DataFill: attribute %d has len %d",
						 i, att[i]->attlen);
				if (att[i]->attalign == 'd')
				{
					data = (char *) DOUBLEALIGN(data);
					memmove(data, DatumGetPointer(value[i]),
							att[i]->attlen);
					data += att[i]->attlen;
				}
				else
				{
					data = (char *) LONGALIGN(data);
					memmove(data, DatumGetPointer(value[i]),
							att[i]->attlen);
					data += att[i]->attlen;
				}
				break;
		}
	}
}

/* ----------------------------------------------------------------
 *						heap tuple interface
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_attisnull	- returns 1 iff tuple attribute is not present
 * ----------------
 */
int
heap_attisnull(HeapTuple tup, int attnum)
{
	if (attnum > (int) tup->t_natts)
		return (1);

	if (HeapTupleNoNulls(tup))
		return (0);

	if (attnum > 0)
	{
		return (att_isnull(attnum - 1, tup->t_bits));
	}
	else
		switch (attnum)
		{
			case SelfItemPointerAttributeNumber:
			case ObjectIdAttributeNumber:
			case MinTransactionIdAttributeNumber:
			case MinCommandIdAttributeNumber:
			case MaxTransactionIdAttributeNumber:
			case MaxCommandIdAttributeNumber:
				break;

			case 0:
				elog(ERROR, "heap_attisnull: zero attnum disallowed");

			default:
				elog(ERROR, "heap_attisnull: undefined negative attnum");
		}

	return (0);
}

/* ----------------------------------------------------------------
 *				 system attribute heap tuple support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_sysattrlen
 *
 *		This routine returns the length of a system attribute.
 * ----------------
 */
int
heap_sysattrlen(AttrNumber attno)
{
	HeapTupleData *f = NULL;

	switch (attno)
	{
		case SelfItemPointerAttributeNumber:
			return sizeof f->t_ctid;
		case ObjectIdAttributeNumber:
			return sizeof f->t_oid;
		case MinTransactionIdAttributeNumber:
			return sizeof f->t_xmin;
		case MinCommandIdAttributeNumber:
			return sizeof f->t_cmin;
		case MaxTransactionIdAttributeNumber:
			return sizeof f->t_xmax;
		case MaxCommandIdAttributeNumber:
			return sizeof f->t_cmax;

		default:
			elog(ERROR, "sysattrlen: System attribute number %d unknown.", attno);
			return 0;
	}
}

/* ----------------
 *		heap_sysattrbyval
 *
 *		This routine returns the "by-value" property of a system attribute.
 * ----------------
 */
bool
heap_sysattrbyval(AttrNumber attno)
{
	bool		byval;

	switch (attno)
	{
		case SelfItemPointerAttributeNumber:
			byval = false;
			break;
		case ObjectIdAttributeNumber:
			byval = true;
			break;
		case MinTransactionIdAttributeNumber:
			byval = true;
			break;
		case MinCommandIdAttributeNumber:
			byval = true;
			break;
		case MaxTransactionIdAttributeNumber:
			byval = true;
			break;
		case MaxCommandIdAttributeNumber:
			byval = true;
			break;
		default:
			byval = true;
			elog(ERROR, "sysattrbyval: System attribute number %d unknown.",
				 attno);
			break;
	}

	return byval;
}

/* ----------------
 *		heap_getsysattr
 * ----------------
 */
Datum
heap_getsysattr(HeapTuple tup, Buffer b, int attnum)
{
	switch (attnum)
	{
		case  SelfItemPointerAttributeNumber:
			return ((Datum) &tup->t_ctid);
		case ObjectIdAttributeNumber:
			return ((Datum) (long) tup->t_oid);
		case MinTransactionIdAttributeNumber:
			return ((Datum) (long) tup->t_xmin);
		case MinCommandIdAttributeNumber:
			return ((Datum) (long) tup->t_cmin);
		case MaxTransactionIdAttributeNumber:
			return ((Datum) (long) tup->t_xmax);
		case MaxCommandIdAttributeNumber:
			return ((Datum) (long) tup->t_cmax);
		default:
			elog(ERROR, "heap_getsysattr: undefined attnum %d", attnum);
	}
	return ((Datum) NULL);
}

/* ----------------
 *		nocachegetattr
 *
 *		This only gets called from fastgetattr() macro, in cases where
 *		we can't use a cacheoffset and the value is not null.
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
nocachegetattr(HeapTuple tup,
			int attnum,
			TupleDesc tupleDesc,
			bool *isnull)
{
	char	   *tp;				/* ptr to att in tuple */
	bits8	   *bp = NULL;		/* ptr to att in tuple */
	int			slow;			/* do we have to walk nulls? */
	AttributeTupleForm *att = tupleDesc->attrs;

	
#if IN_MACRO
/* This is handled in the macro */
	Assert(attnum > 0);

	if (isnull)
		*isnull = false;
#endif

	/* ----------------
	 *	 Three cases:
	 *
	 *	 1: No nulls and no variable length attributes.
	 *	 2: Has a null or a varlena AFTER att.
	 *	 3: Has nulls or varlenas BEFORE att.
	 * ----------------
	 */

	if (HeapTupleNoNulls(tup))
	{
		attnum--;

#if IN_MACRO
/* This is handled in the macro */
		if (att[attnum]->attcacheoff > 0)
		{
			return (Datum)
				fetchatt(&(att[attnum]),
				  (char *) tup + tup->t_hoff + att[attnum]->attcacheoff);
		}
		else if (attnum == 0)
		{

			/*
			 * first attribute is always at position zero
			 */
			return ((Datum) fetchatt(&(att[0]), (char *) tup + tup->t_hoff));
		}
#endif

		tp = (char *) tup + tup->t_hoff;

		slow = 0;
	}
	else
	{

		/*
		 * there's a null somewhere in the tuple
		 */

		bp = tup->t_bits;
		tp = (char *) tup + tup->t_hoff;
		slow = 0;
		attnum--;

		/* ----------------
		 *		check to see if desired att is null
		 * ----------------
		 */

#if IN_MACRO
/* This is handled in the macro */
		if (att_isnull(attnum, bp))
		{
			if (isnull)
				*isnull = true;
			return (Datum) NULL;
		}
#endif

		/* ----------------
		 *		Now check to see if any preceeding bits are null...
		 * ----------------
		 */

		{
			register int i = 0; /* current offset in bp */

			for (i = 0; i < attnum && !slow; i++)
			{
				if (att_isnull(i, bp))
					slow = 1;
			}
		}
	}

	/*
	 * now check for any non-fixed length attrs before our attribute
	 */
	if (!slow)
	{
		if (att[attnum]->attcacheoff > 0)
		{
			return (Datum)
				fetchatt(&(att[attnum]),
						 tp + att[attnum]->attcacheoff);
		}
		else if (attnum == 0)
		{
			return (Datum)
				fetchatt(&(att[0]), (char *) tup + tup->t_hoff);
		}
		else if (!HeapTupleAllFixed(tup))
		{
			register int j = 0;

			for (j = 0; j < attnum && !slow; j++)
				if (att[j]->attlen < 1)
					slow = 1;
		}
	}

	/*
	 * if slow is zero, and we got here, we know that we have a tuple with
	 * no nulls.  We also have to initialize the remainder of the
	 * attribute cached offset values.
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

		off = att[j - 1]->attcacheoff + att[j - 1]->attlen;

		for (; j < attnum + 1; j++)
		{
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
					if (att[j]->attlen < sizeof(int32))
					{
						elog(ERROR,
							 "nocachegetattr: attribute %d has len %d",
							 j, att[j]->attlen);
					}
					if (att[j]->attalign == 'd')
						off = DOUBLEALIGN(off);
					else
						off = LONGALIGN(off);
					break;
			}

			att[j]->attcacheoff = off;
			off += att[j]->attlen;
		}

		return
			(Datum) fetchatt(&(att[attnum]), tp + att[attnum]->attcacheoff);
	}
	else
	{
		register bool usecache = true;
		register int off = 0;
		register int i;

		/*
		 * Now we know that we have to walk the tuple CAREFULLY.
		 *
		 * Note - This loop is a little tricky.  On iteration i we first set
		 * the offset for attribute i and figure out how much the offset
		 * should be incremented.  Finally, we need to align the offset
		 * based on the size of attribute i+1 (for which the offset has
		 * been computed). -mer 12 Dec 1991
		 */

		for (i = 0; i < attnum; i++)
		{
			if (!HeapTupleNoNulls(tup))
			{
				if (att_isnull(i, bp))
				{
					usecache = false;
					continue;
				}
			}
			switch (att[i]->attlen)
			{
				case -1:
					off = (att[i]->attalign == 'd') ?
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
					if (att[i]->attlen < sizeof(int32))
						elog(ERROR,
							 "nocachegetattr2: attribute %d has len %d",
							 i, att[i]->attlen);
					if (att[i]->attalign == 'd')
						off = DOUBLEALIGN(off);
					else
						off = LONGALIGN(off);
					break;
			}
			if (usecache && att[i]->attcacheoff > 0)
			{
				off = att[i]->attcacheoff;
				if (att[i]->attlen == -1)
				{
					usecache = false;
				}
			}
			else
			{
				if (usecache)
					att[i]->attcacheoff = off;
			}

			switch (att[i]->attlen)
			{
				case sizeof(char):
					off++;
					break;
				case sizeof(int16):
					off += sizeof(int16);
					break;
				case sizeof(int32):
					off += sizeof(int32);
					break;
				case -1:
					usecache = false;
					off += VARSIZE(tp + off);
					break;
				default:
					off += att[i]->attlen;
					break;
			}
		}
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
					elog(ERROR, "nocachegetattr3: attribute %d has len %d",
						 attnum, att[attnum]->attlen);
				if (att[attnum]->attalign == 'd')
					off = DOUBLEALIGN(off);
				else
					off = LONGALIGN(off);
				break;
		}
		return ((Datum) fetchatt(&(att[attnum]), tp + off));
	}
}

/* ----------------
 *		heap_copytuple
 *
 *		returns a copy of an entire tuple
 * ----------------
 */
HeapTuple
heap_copytuple(HeapTuple tuple)
{
	HeapTuple	newTuple;

	if (!HeapTupleIsValid(tuple))
		return (NULL);

	/* XXX For now, just prevent an undetectable executor related error */
	if (tuple->t_len > MAXTUPLEN)
	{
		elog(ERROR, "palloctup: cannot handle length %d tuples",
			 tuple->t_len);
	}

	newTuple = (HeapTuple) palloc(tuple->t_len);
	memmove((char *) newTuple, (char *) tuple, (int) tuple->t_len);
	return (newTuple);
}

#ifdef NOT_USED
/* ----------------
 *		heap_deformtuple
 *
 *		the inverse of heap_formtuple (see below)
 * ----------------
 */
void
heap_deformtuple(HeapTuple tuple,
				 TupleDesc tdesc,
				 Datum values[],
				 char nulls[])
{
	int			i;
	int			natts;

	Assert(HeapTupleIsValid(tuple));

	natts = tuple->t_natts;
	for (i = 0; i < natts; i++)
	{
		bool		isnull;

		values[i] = heap_getattr(tuple,
								 i + 1,
								 tdesc,
								 &isnull);
		if (isnull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}
}

#endif

/* ----------------
 *		heap_formtuple
 *
 *		constructs a tuple from the given value[] and null[] arrays
 *
 * old comments
 *		Handles alignment by aligning 2 byte attributes on short boundries
 *		and 3 or 4 byte attributes on long word boundries on a vax; and
 *		aligning non-byte attributes on short boundries on a sun.  Does
 *		not properly align fixed length arrays of 1 or 2 byte types (yet).
 *
 *		Null attributes are indicated by a 'n' in the appropriate byte
 *		of the null[].	Non-null attributes are indicated by a ' ' (space).
 *
 *		Fix me.  (Figure that must keep context if debug--allow give oid.)
 *		Assumes in order.
 * ----------------
 */
HeapTuple
heap_formtuple(TupleDesc tupleDescriptor,
			   Datum value[],
			   char nulls[])
{
	char	   *tp;				/* tuple pointer */
	HeapTuple	tuple;			/* return tuple */
	int			bitmaplen;
	long		len;
	int			hoff;
	bool		hasnull = false;
	int			i;
	int			numberOfAttributes = tupleDescriptor->natts;

	len = sizeof *tuple - sizeof tuple->t_bits;

	for (i = 0; i < numberOfAttributes && !hasnull; i++)
	{
		if (nulls[i] != ' ')
			hasnull = true;
	}

	if (numberOfAttributes > MaxHeapAttributeNumber)
		elog(ERROR, "heap_formtuple: numberOfAttributes of %d > %d",
			 numberOfAttributes, MaxHeapAttributeNumber);

	if (hasnull)
	{
		bitmaplen = BITMAPLEN(numberOfAttributes);
		len += bitmaplen;
	}

	hoff = len = DOUBLEALIGN(len);		/* be conservative here */

	len += ComputeDataSize(tupleDescriptor, value, nulls);

	tp = (char *) palloc(len);
	tuple = (HeapTuple) tp;

	MemSet(tp, 0, (int) len);

	tuple->t_len = len;
	tuple->t_natts = numberOfAttributes;
	tuple->t_hoff = hoff;

	DataFill((char *) tuple + tuple->t_hoff,
			 tupleDescriptor,
			 value,
			 nulls,
			 &tuple->t_infomask,
			 (hasnull ? tuple->t_bits : NULL));

	tuple->t_infomask |= HEAP_XMAX_INVALID;

	return (tuple);
}

/* ----------------
 *		heap_modifytuple
 *
 *		forms a new tuple from an old tuple and a set of replacement values.
 * ----------------
 */
HeapTuple
heap_modifytuple(HeapTuple tuple,
				 Buffer buffer,
				 Relation relation,
				 Datum replValue[],
				 char replNull[],
				 char repl[])
{
	int			attoff;
	int			numberOfAttributes;
	Datum	   *value;
	char	   *nulls;
	bool		isNull;
	HeapTuple	newTuple;
	int			madecopy;
	uint8		infomask;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(HeapTupleIsValid(tuple));
	Assert(BufferIsValid(buffer) || RelationIsValid(relation));
	Assert(HeapTupleIsValid(tuple));
	Assert(PointerIsValid(replValue));
	Assert(PointerIsValid(replNull));
	Assert(PointerIsValid(repl));

	/* ----------------
	 *	if we're pointing to a disk page, then first
	 *	make a copy of our tuple so that all the attributes
	 *	are available.	XXX this is inefficient -cim
	 * ----------------
	 */
	madecopy = 0;
	if (BufferIsValid(buffer) == true)
	{
		relation = (Relation) BufferGetRelation(buffer);
		tuple = heap_copytuple(tuple);
		madecopy = 1;
	}

	numberOfAttributes = RelationGetRelationTupleForm(relation)->relnatts;

	/* ----------------
	 *	allocate and fill value[] and nulls[] arrays from either
	 *	the tuple or the repl information, as appropriate.
	 * ----------------
	 */
	value = (Datum *) palloc(numberOfAttributes * sizeof *value);
	nulls = (char *) palloc(numberOfAttributes * sizeof *nulls);

	for (attoff = 0;
		 attoff < numberOfAttributes;
		 attoff += 1)
	{

		if (repl[attoff] == ' ')
		{
			value[attoff] =
				heap_getattr(tuple,
							 AttrOffsetGetAttrNumber(attoff),
							 RelationGetTupleDescriptor(relation),
							 &isNull);
			nulls[attoff] = (isNull) ? 'n' : ' ';

		}
		else if (repl[attoff] != 'r')
		{
			elog(ERROR, "heap_modifytuple: repl is \\%3d", repl[attoff]);

		}
		else
		{						/* == 'r' */
			value[attoff] = replValue[attoff];
			nulls[attoff] = replNull[attoff];
		}
	}

	/* ----------------
	 *	create a new tuple from the values[] and nulls[] arrays
	 * ----------------
	 */
	newTuple = heap_formtuple(RelationGetTupleDescriptor(relation),
							  value,
							  nulls);

	/* ----------------
	 *	copy the header except for t_len, t_natts, t_hoff, t_bits, t_infomask
	 * ----------------
	 */
	infomask = newTuple->t_infomask;
	memmove((char *) &newTuple->t_oid, /* XXX */
			(char *) &tuple->t_oid,
			((char *) &tuple->t_hoff - (char *) &tuple->t_oid));	/* XXX */
	newTuple->t_infomask = infomask;
	newTuple->t_natts = numberOfAttributes;		/* fix t_natts just in
												 * case */

	/* ----------------
	 *	if we made a copy of the tuple, then free it.
	 * ----------------
	 */
	if (madecopy)
		pfree(tuple);

	return
		newTuple;
}

/* ----------------------------------------------------------------
 *						other misc functions
 * ----------------------------------------------------------------
 */

HeapTuple
heap_addheader(uint32 natts,	/* max domain index */
			   int structlen,	/* its length */
			   char *structure) /* pointer to the struct */
{
	register char *tp;			/* tuple data pointer */
	HeapTuple	tup;
	long		len;
	int			hoff;

	AssertArg(natts > 0);

	len = sizeof(HeapTupleData) - sizeof(tup->t_bits);

	hoff = len = DOUBLEALIGN(len);		/* be conservative */
	len += structlen;
	tp = (char *) palloc(len);
	tup = (HeapTuple) tp;
	MemSet((char *) tup, 0, len);

	tup->t_len = len;
	tp += tup->t_hoff = hoff;
	tup->t_natts = natts;
	tup->t_infomask = 0;
	tup->t_infomask |= HEAP_XMAX_INVALID;

	memmove(tp, structure, structlen);

	return (tup);
}


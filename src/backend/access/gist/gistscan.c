/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on GiST index relations
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/* routines defined and used here */
static void gistregscan(IndexScanDesc scan);
static void gistdropscan(IndexScanDesc scan);
static void gistadjone(IndexScanDesc scan, int op, BlockNumber blkno,
		   OffsetNumber offnum);
static void adjuststack(GISTSTACK *stk, BlockNumber blkno);
static void adjustiptr(IndexScanDesc scan, ItemPointer iptr,
		   int op, BlockNumber blkno, OffsetNumber offnum);
static void gistfreestack(GISTSTACK *s);

/*
 * Whenever we start a GiST scan in a backend, we register it in
 * private space. Then if the GiST index gets updated, we check all
 * registered scans and adjust them if the tuple they point at got
 * moved by the update.  We only need to do this in private space,
 * because when we update an GiST we have a write lock on the tree, so
 * no other process can have any locks at all on it.  A single
 * transaction can have write and read locks on the same object, so
 * that's why we need to handle this case.
 */
typedef struct GISTScanListData
{
	IndexScanDesc gsl_scan;
	ResourceOwner gsl_owner;
	struct GISTScanListData *gsl_next;
} GISTScanListData;

typedef GISTScanListData *GISTScanList;

/* pointer to list of local scans on GiSTs */
static GISTScanList GISTScans = NULL;

Datum
gistbeginscan(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	scan = RelationGetIndexScan(r, nkeys, key);
	gistregscan(scan);

	PG_RETURN_POINTER(scan);
}

Datum
gistrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(1);
	GISTScanOpaque so;
	int			i;

	/*
	 * Clear all the pointers.
	 */
	ItemPointerSetInvalid(&scan->currentItemData);
	ItemPointerSetInvalid(&scan->currentMarkData);

	so = (GISTScanOpaque) scan->opaque;
	if (so != NULL)
	{
		/* rescan an existing indexscan --- reset state */
		gistfreestack(so->stack);
		gistfreestack(so->markstk);
		so->stack = so->markstk = NULL;
		so->flags = 0x0;
		/* drop pins on buffers -- no locks held */
		if (BufferIsValid(so->curbuf))
		{
			ReleaseBuffer(so->curbuf);
			so->curbuf = InvalidBuffer;
		}
		if (BufferIsValid(so->markbuf))
		{
			ReleaseBuffer(so->markbuf);
			so->markbuf = InvalidBuffer;
		}
	}
	else
	{
		/* initialize opaque data */
		so = (GISTScanOpaque) palloc(sizeof(GISTScanOpaqueData));
		so->stack = so->markstk = NULL;
		so->flags = 0x0;
		so->tempCxt = createTempGistContext();
		so->curbuf = so->markbuf = InvalidBuffer;
		so->giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));
		initGISTstate(so->giststate, scan->indexRelation);

		scan->opaque = so;
	}

	/* Update scan key, if a new one is given */
	if (key && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, key,
				scan->numberOfKeys * sizeof(ScanKeyData));

		/*
		 * Modify the scan key so that all the Consistent method is
		 * called for all comparisons. The original operator is passed
		 * to the Consistent function in the form of its strategy
		 * number, which is available from the sk_strategy field, and
		 * its subtype from the sk_subtype field.
		 */
		for (i = 0; i < scan->numberOfKeys; i++)
			scan->keyData[i].sk_func = so->giststate->consistentFn[scan->keyData[i].sk_attno - 1];
	}

	PG_RETURN_VOID();
}

Datum
gistmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so;
	GISTSTACK  *o,
			   *n,
			   *tmp;

	scan->currentMarkData = scan->currentItemData;
	so = (GISTScanOpaque) scan->opaque;
	if (so->flags & GS_CURBEFORE)
		so->flags |= GS_MRKBEFORE;
	else
		so->flags &= ~GS_MRKBEFORE;

	o = NULL;
	n = so->stack;

	/* copy the parent stack from the current item data */
	while (n != NULL)
	{
		tmp = (GISTSTACK *) palloc(sizeof(GISTSTACK));
		tmp->offset = n->offset;
		tmp->block = n->block;
		tmp->parent = o;
		o = tmp;
		n = n->parent;
	}

	gistfreestack(so->markstk);
	so->markstk = o;

	/* Update markbuf: make sure to bump ref count on curbuf */
	if (BufferIsValid(so->markbuf))
	{
		ReleaseBuffer(so->markbuf);
		so->markbuf = InvalidBuffer;
	}
	if (BufferIsValid(so->curbuf))
	{
		IncrBufferRefCount(so->curbuf);
		so->markbuf = so->curbuf;
	}

	PG_RETURN_VOID();
}

Datum
gistrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so;
	GISTSTACK  *o,
			   *n,
			   *tmp;

	scan->currentItemData = scan->currentMarkData;
	so = (GISTScanOpaque) scan->opaque;
	if (so->flags & GS_MRKBEFORE)
		so->flags |= GS_CURBEFORE;
	else
		so->flags &= ~GS_CURBEFORE;

	o = NULL;
	n = so->markstk;

	/* copy the parent stack from the current item data */
	while (n != NULL)
	{
		tmp = (GISTSTACK *) palloc(sizeof(GISTSTACK));
		tmp->offset = n->offset;
		tmp->block = n->block;
		tmp->parent = o;
		o = tmp;
		n = n->parent;
	}

	gistfreestack(so->stack);
	so->stack = o;

	/* Update curbuf: be sure to bump ref count on markbuf */
	if (BufferIsValid(so->curbuf))
	{
		ReleaseBuffer(so->curbuf);
		so->curbuf = InvalidBuffer;
	}
	if (BufferIsValid(so->markbuf))
	{
		IncrBufferRefCount(so->markbuf);
		so->curbuf = so->markbuf;
	}

	PG_RETURN_VOID();
}

Datum
gistendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque so;

	so = (GISTScanOpaque) scan->opaque;

	if (so != NULL)
	{
		gistfreestack(so->stack);
		gistfreestack(so->markstk);
		if (so->giststate != NULL)
			freeGISTstate(so->giststate);
		/* drop pins on buffers -- we aren't holding any locks */
		if (BufferIsValid(so->curbuf))
			ReleaseBuffer(so->curbuf);
		if (BufferIsValid(so->markbuf))
			ReleaseBuffer(so->markbuf);
		MemoryContextDelete(so->tempCxt);
		pfree(scan->opaque);
	}

	gistdropscan(scan);

	PG_RETURN_VOID();
}

static void
gistregscan(IndexScanDesc scan)
{
	GISTScanList l;

	l = (GISTScanList) palloc(sizeof(GISTScanListData));
	l->gsl_scan = scan;
	l->gsl_owner = CurrentResourceOwner;
	l->gsl_next = GISTScans;
	GISTScans = l;
}

static void
gistdropscan(IndexScanDesc scan)
{
	GISTScanList l;
	GISTScanList prev;

	prev = NULL;

	for (l = GISTScans; l != NULL && l->gsl_scan != scan; l = l->gsl_next)
		prev = l;

	if (l == NULL)
		elog(ERROR, "GiST scan list corrupted -- could not find 0x%p",
			 (void *) scan);

	if (prev == NULL)
		GISTScans = l->gsl_next;
	else
		prev->gsl_next = l->gsl_next;

	pfree(l);
}

/*
 * ReleaseResources_gist() --- clean up gist subsystem resources.
 *
 * This is here because it needs to touch this module's static var GISTScans.
 */
void
ReleaseResources_gist(void)
{
	GISTScanList l;
	GISTScanList prev;
	GISTScanList next;

	/*
	 * Note: this should be a no-op during normal query shutdown. However,
	 * in an abort situation ExecutorEnd is not called and so there may be
	 * open index scans to clean up.
	 */
	prev = NULL;

	for (l = GISTScans; l != NULL; l = next)
	{
		next = l->gsl_next;
		if (l->gsl_owner == CurrentResourceOwner)
		{
			if (prev == NULL)
				GISTScans = next;
			else
				prev->gsl_next = next;

			pfree(l);
			/* prev does not change */
		}
		else
			prev = l;
	}
}

void
gistadjscans(Relation rel, int op, BlockNumber blkno, OffsetNumber offnum)
{
	GISTScanList l;
	Oid			relid;

	relid = RelationGetRelid(rel);
	for (l = GISTScans; l != NULL; l = l->gsl_next)
	{
		if (l->gsl_scan->indexRelation->rd_id == relid)
			gistadjone(l->gsl_scan, op, blkno, offnum);
	}
}

/*
 *	gistadjone() -- adjust one scan for update.
 *
 *		By here, the scan passed in is on a modified relation.	Op tells
 *		us what the modification is, and blkno and offind tell us what
 *		block and offset index were affected.  This routine checks the
 *		current and marked positions, and the current and marked stacks,
 *		to see if any stored location needs to be changed because of the
 *		update.  If so, we make the change here.
 */
static void
gistadjone(IndexScanDesc scan,
		   int op,
		   BlockNumber blkno,
		   OffsetNumber offnum)
{
	GISTScanOpaque so;

	adjustiptr(scan, &(scan->currentItemData), op, blkno, offnum);
	adjustiptr(scan, &(scan->currentMarkData), op, blkno, offnum);

	so = (GISTScanOpaque) scan->opaque;

	if (op == GISTOP_SPLIT)
	{
		adjuststack(so->stack, blkno);
		adjuststack(so->markstk, blkno);
	}
}

/*
 *	adjustiptr() -- adjust current and marked item pointers in the scan
 *
 *		Depending on the type of update and the place it happened, we
 *		need to do nothing, to back up one record, or to start over on
 *		the same page.
 */
static void
adjustiptr(IndexScanDesc scan,
		   ItemPointer iptr,
		   int op,
		   BlockNumber blkno,
		   OffsetNumber offnum)
{
	OffsetNumber curoff;
	GISTScanOpaque so;

	if (ItemPointerIsValid(iptr))
	{
		if (ItemPointerGetBlockNumber(iptr) == blkno)
		{
			curoff = ItemPointerGetOffsetNumber(iptr);
			so = (GISTScanOpaque) scan->opaque;

			switch (op)
			{
				case GISTOP_DEL:
					/* back up one if we need to */
					if (curoff >= offnum)
					{
						if (curoff > FirstOffsetNumber)
						{
							/* just adjust the item pointer */
							ItemPointerSet(iptr, blkno, OffsetNumberPrev(curoff));
						}
						else
						{
							/*
							 * remember that we're before the current
							 * tuple
							 */
							ItemPointerSet(iptr, blkno, FirstOffsetNumber);
							if (iptr == &(scan->currentItemData))
								so->flags |= GS_CURBEFORE;
							else
								so->flags |= GS_MRKBEFORE;
						}
					}
					break;

				case GISTOP_SPLIT:
					/* back to start of page on split */
					ItemPointerSet(iptr, blkno, FirstOffsetNumber);
					if (iptr == &(scan->currentItemData))
						so->flags &= ~GS_CURBEFORE;
					else
						so->flags &= ~GS_MRKBEFORE;
					break;

				default:
					elog(ERROR, "Bad operation in GiST scan adjust: %d", op);
			}
		}
	}
}

/*
 *	adjuststack() -- adjust the supplied stack for a split on a page in
 *					 the index we're scanning.
 *
 *		If a page on our parent stack has split, we need to back up to the
 *		beginning of the page and rescan it.  The reason for this is that
 *		the split algorithm for GiSTs doesn't order tuples in any useful
 *		way on a single page.  This means on that a split, we may wind up
 *		looking at some heap tuples more than once.  This is handled in the
 *		access method update code for heaps; if we've modified the tuple we
 *		are looking at already in this transaction, we ignore the update
 *		request.
 */
static void
adjuststack(GISTSTACK *stk, BlockNumber blkno)
{
	while (stk != NULL)
	{
		if (stk->block == blkno)
			stk->offset = FirstOffsetNumber;

		stk = stk->parent;
	}
}

static void
gistfreestack(GISTSTACK *s)
{
	while (s != NULL)
	{
		GISTSTACK *p = s->parent;
		pfree(s);
		s = p;
	}
}

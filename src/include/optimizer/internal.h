/*-------------------------------------------------------------------------
 *
 * internal.h
 *	  Definitions required throughout the query optimizer.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef INTERNAL_H
#define INTERNAL_H

#include "catalog/pg_index.h"

/*
 *		---------- SHARED MACROS
 *
 *		Macros common to modules for creating, accessing, and modifying
 *		query tree and query plan components.
 *		Shared with the executor.
 *
 */


/*
 *		System-dependent tuning constants
 *
 */
#define _CPU_PAGE_WEIGHT_  0.033  /* CPU-heap-to-page cost weighting factor */
#define _CPU_INDEX_PAGE_WEIGHT_ 0.017	/* CPU-index-to-page cost
										 * weighting factor */
#define _MAX_KEYS_	   INDEX_MAX_KEYS	/* maximum number of keys in an
										 * index */
#define _TID_SIZE_	   6		/* sizeof(itemid) (from ../h/itemid.h) */

/*
 *		Size estimates
 *
 */

/*	   The cost of sequentially scanning a materialized temporary relation
 */
#define _NONAME_SCAN_COST_		10

/*	   The number of pages and tuples in a materialized relation
 */
#define _NONAME_RELATION_PAGES_			1
#define _NONAME_RELATION_TUPLES_	10

/*	   The length of a variable-length field in bytes
 */
#define _DEFAULT_ATTRIBUTE_WIDTH_ (2 * _TID_SIZE_)

/*
 *		Flags and identifiers
 *
 */

/*	   Identifier for (sort) temp relations   */
/* used to be -1 */
#define _NONAME_RELATION_ID_	 InvalidOid

/* #define deactivate_joininfo(joininfo)		joininfo->inactive=true*/
/*#define joininfo_inactive(joininfo)	joininfo->inactive */

/* GEQO switch according to number of relations in a query */
#define GEQO_RELS 11

#endif	 /* INTERNAL_H */

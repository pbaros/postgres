/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

#include "nodes/parsenodes.h"


extern void RemoveRewriteRule(Oid owningRel, const char *ruleName,
				  DropBehavior behavior);
extern void RemoveRewriteRuleById(Oid ruleOid);

#endif   /* REWRITEREMOVE_H */

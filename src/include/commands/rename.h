/*-------------------------------------------------------------------------
 *
 * rename.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef RENAME_H
#define RENAME_H

extern void renameatt(Oid relid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse);

extern void renamerel(Oid relid,
		  const char *newrelname);

#endif   /* RENAME_H */

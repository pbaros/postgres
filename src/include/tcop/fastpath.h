/*-------------------------------------------------------------------------
 *
 * fastpath.h
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef FASTPATH_H
#define FASTPATH_H

#include "lib/stringinfo.h"

extern int	HandleFunctionRequest(StringInfo msgBuf);

#endif   /* FASTPATH_H */

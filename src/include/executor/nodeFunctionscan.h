/*-------------------------------------------------------------------------
 *
 * nodeFunctionscan.h
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
#ifndef NODEFUNCTIONSCAN_H
#define NODEFUNCTIONSCAN_H

#include "nodes/execnodes.h"

extern int	ExecCountSlotsFunctionScan(FunctionScan *node);
extern FunctionScanState *ExecInitFunctionScan(FunctionScan *node, EState *estate);
extern TupleTableSlot *ExecFunctionScan(FunctionScanState *node);
extern void ExecEndFunctionScan(FunctionScanState *node);
extern void ExecFunctionMarkPos(FunctionScanState *node);
extern void ExecFunctionRestrPos(FunctionScanState *node);
extern void ExecFunctionReScan(FunctionScanState *node, ExprContext *exprCtxt);

#endif   /* NODEFUNCTIONSCAN_H */

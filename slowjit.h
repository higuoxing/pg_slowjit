#ifndef _SLOWJIT_H_
#define _SLOWJIT_H_

#include "postgres.h"

#include "executor/execExpr.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "port.h"
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "utils/expandeddatum.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "utils/resowner_private.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern char *slowjit_cc_path;

extern void _PG_init(void);
extern void _PG_jit_provider_init(JitProviderCallbacks *cb);

typedef struct SlowjitContext {
  JitContext base;
  StringInfoData code_holder;
  /* Is there any pending code that needs to be emitted */
  bool compiled;
  /* # of objects emitted, used to generate non-conflicting names */
  int counter;

  size_t module_generation;
  /* Handles of the compiled shared libraries. */
  List *handles;
} SlowJitContext;

typedef struct SlowJitCompiledExprState {
  SlowJitContext *jit_ctx;
  const char *funcname;
} SlowJitCompiledExprState;

extern bool slowjit_compile_expr(ExprState *state);
extern void slowjit_release_context(JitContext *jit_ctx);
extern void slowjit_reset_after_error(void);

#endif

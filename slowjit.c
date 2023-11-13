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
#include <fcntl.h>
#include <stdio.h>

PG_MODULE_MAGIC;

static int module_generation = 0;

extern void _PG_jit_provider_init(JitProviderCallbacks *cb);

typedef struct SlowJitHandle {
  /* Shared library path, for error reporting. */
  char *shared_library_path;
  /* Handle of the shared library. */
  void *handle;
} SlowJitHandle;

typedef struct SlowJitContext {
  JitContext base;
  /* Handles of the compiled shared libraries. */
  List *handles;
} SlowJitContext;

static bool slowjit_compile_expr(ExprState *state) {
  PlanState *parent = state->parent;
  SlowJitContext *jit_ctx = NULL;
  StringInfoData code_holder;
  char symbol_name[MAXPGPATH];

  /* parent shouldn't be NULL. */
  Assert(parent != NULL);

  /* Initialize the context. */
  if (parent->state->es_jit) {
    jit_ctx = (SlowJitContext *)parent->state->es_jit;
  } else {
    ResourceOwnerEnlargeJIT(CurrentResourceOwner);

    jit_ctx = (SlowJitContext *)MemoryContextAllocZero(TopMemoryContext,
                                                       sizeof(SlowJitContext));
    jit_ctx->base.flags = parent->state->es_jit_flags;

    /* ensure cleanup */
    jit_ctx->base.resowner = CurrentResourceOwner;
    ResourceOwnerRememberJIT(CurrentResourceOwner, PointerGetDatum(jit_ctx));

    parent->state->es_jit = &jit_ctx->base;
  }

  initStringInfo(&code_holder);

#define emit_line(...)                                                         \
  do {                                                                         \
    appendStringInfo(&code_holder, __VA_ARGS__);                               \
    appendStringInfoChar(&code_holder, '\n');                                  \
  } while (0)

#define emit_include(header) emit_line("#include \"%s\"", header)

  emit_include("postgres.h");
  emit_include("nodes/execnodes.h");

  /* Emit the jitted function signature. */
  snprintf(symbol_name, MAXPGPATH, "slowjit_eval_expr_%d_%d", MyProcPid,
           module_generation);
  emit_line("Datum %s(ExprState *state, ExprContext *econtext, bool *isnull)",
            symbol_name);

  /* Open function body. */
  emit_line("{");

  /* Emit some commonly used variables. */
  emit_line("  TupleTableSlot *resultslot = state->resultslot;");

  for (int opno = 0; opno < state->steps_len; ++opno) {
    ExprEvalStep *op;
    ExprEvalOp opcode;

    op = &state->steps[opno];
    opcode = ExecEvalStepOp(state, op);

    switch (opcode) {
    case EEOP_DONE: {
      emit_line("  { // EEOP_DONE");
      emit_line("    *isnull = state->resnull;");
      emit_line("  }");
      emit_line("  return state->resvalue;");

      /* Close function boday. */
      emit_line("}");
      break;
    }
    case EEOP_ASSIGN_TMP: {
      int resultnum = op->d.assign_tmp.resultnum;
      emit_line("  { // EEOP_ASSIGN_TMP");
      emit_line("    resultslot->tts_values[%d] = state->resvalue;", resultnum);
      emit_line("    resultslot->tts_isnull[%d] = state->resnull;", resultnum);
      emit_line("  }");
      break;
    }
    case EEOP_CONST: {
      emit_line("  { // EEOP_CONST");
      emit_line("    bool *resnull = (bool *) %lu;", (uint64_t)op->resnull);
      emit_line("    Datum *resvalue = (Datum *) %lu;", (uint64_t)op->resvalue);
      emit_line("    *resnull = (bool) %d;", op->d.constval.isnull);
      emit_line("    *resvalue = (Datum) %luull;", op->d.constval.value);
      emit_line("  }");
      break;
    }
    default: {
      resetStringInfo(&code_holder);
      pfree(code_holder.data);
      return false;
    }
    }
  }

  {
    char c_src_path[MAXPGPATH];
    char shared_library_path[MAXPGPATH];
    char include_server_path[MAXPGPATH];
    char compile_command[MAXPGPATH];
    FILE *c_src_file;
    void *handle;
    void *jitted_func;
    MemoryContext oldctx;

    /* Write the emitted C codes to a file. */
    snprintf(c_src_path, MAXPGPATH, "/tmp/%d.%d.c", MyProcPid,
             module_generation);
    c_src_file = fopen(c_src_path, "w+");
    if (c_src_file == NULL) {
      ereport(ERROR, (errmsg("cannot open file '%s' for write", c_src_path)));
    }
    fwrite(code_holder.data, 1, code_holder.len, c_src_file);
    fclose(c_src_file);
    resetStringInfo(&code_holder);
    pfree(code_holder.data);

    /* Prepare the compile command. */
    snprintf(shared_library_path, MAXPGPATH, "/tmp/%d.%d.so", MyProcPid,
             module_generation);
    get_includeserver_path(my_exec_path, include_server_path);
    snprintf(compile_command, MAXPGPATH,
             "cc -fPIC -I%s -shared -O0 -ggdb -g3 -o %s %s",
             include_server_path, shared_library_path, c_src_path);

    /* Compile the codes */
    if (system(compile_command) != 0) {
      ereport(ERROR, (errmsg("cannot execute command: %s", compile_command)));
    }

    /* Load the shared library to the current process. */
    handle = dlopen(shared_library_path, RTLD_LAZY);
    if (handle == NULL) {
      char *err = dlerror();
      ereport(ERROR,
              (errmsg("cannot dlopen '%s': %s", shared_library_path, err)));
    }

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    jit_ctx->handles = lappend(jit_ctx->handles, handle);
    MemoryContextSwitchTo(oldctx);

    /* Find the function pointer and save it to state->evalfunc */
    jitted_func = dlsym(handle, symbol_name);
    if (jitted_func == NULL) {
      char *err = dlerror();
      ereport(ERROR, (errmsg("cannot find symbol '%s' from '%s': %s",
                             symbol_name, shared_library_path, err)));
    }

    state->evalfunc = jitted_func;
    state->evalfunc_private = NULL;
    module_generation++;
  }

  return true;
}

static void slowjit_release_context(JitContext *ctx) {
  SlowJitContext *jit_ctx = (SlowJitContext *)ctx;
  ListCell *lc;

  foreach (lc, jit_ctx->handles) {
    void *handle = (void *)lfirst(lc);
    dlclose(handle);
  }
  list_free(jit_ctx->handles);
  jit_ctx->handles = NIL;
}

static void slowjit_reset_after_error(void) {}

void _PG_jit_provider_init(JitProviderCallbacks *cb) {
  cb->compile_expr = slowjit_compile_expr;
  cb->release_context = slowjit_release_context;
  cb->reset_after_error = slowjit_reset_after_error;
}

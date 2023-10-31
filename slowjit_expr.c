#include "slowjit.h"

static size_t slowjit_module_generation = 0;

static SlowJitContext *slowjit_create_context(int jit_flags) {
  SlowJitContext *jit_ctx = NULL;
  MemoryContext oldcontext;

  ResourceOwnerEnlargeJIT(CurrentResourceOwner);

  jit_ctx = MemoryContextAllocZero(TopMemoryContext, sizeof(SlowJitContext));
  jit_ctx->base.flags = jit_flags;

  /* We emit the whole C program to the TopMemoryContext. */
  oldcontext = MemoryContextSwitchTo(TopMemoryContext);
  initStringInfo(&jit_ctx->code_holder);
  MemoryContextSwitchTo(oldcontext);

  /* ensure cleanup */
  jit_ctx->base.resowner = CurrentResourceOwner;
  ResourceOwnerRememberJIT(CurrentResourceOwner, PointerGetDatum(jit_ctx));

  return jit_ctx;
}

/* This function is copied and pasted from llvmjit. */
static char *slowjit_expand_funcname(SlowJitContext *jit_ctx,
                                     const char *basename) {
  jit_ctx->base.instr.created_functions++;
  return psprintf("%s_%zu_%d", basename, jit_ctx->module_generation,
                  jit_ctx->counter++);
}

static void slowjit_compile_module(SlowJitContext *jit_ctx) {
  char shared_library_path[MAXPGPATH];
  snprintf(shared_library_path, MAXPGPATH, "/tmp/%d.%zu.so", MyProcPid,
           jit_ctx->module_generation);
  {
    char c_src_path[MAXPGPATH];
    /* Compile the program to a shared library. */
    char include_server_path[MAXPGPATH];
    FILE *c_src;
    char command[MAXPGPATH];

    /* Write the C program to a dot C file. */
    snprintf(c_src_path, MAXPGPATH, "/tmp/%d.%zu.c", MyProcPid,
             jit_ctx->module_generation);
    c_src = fopen(c_src_path, "w+");
    if (c_src == NULL) {
      ereport(ERROR, (errmsg("cannot open file '%s' for write", c_src_path)));
    }
    fwrite(jit_ctx->code_holder.data, 1, jit_ctx->code_holder.len, c_src);
    fclose(c_src);

    /* Code holder now is useless. */
    resetStringInfo(&jit_ctx->code_holder);

    /* Prepare compile command. */
    get_includeserver_path(my_exec_path, include_server_path);
    snprintf(command, MAXPGPATH, "%s -fPIC -I%s -shared -o %s %s",
             slowjit_cc_path, include_server_path, shared_library_path,
             c_src_path);
    system(command);
  }

  {
    /* Load the compiled shared library to the backend process. */
    void *handle;
    MemoryContext oldcontext;

    handle = dlopen(shared_library_path, RTLD_LAZY);
    if (handle == NULL) {
      char *err = dlerror();
      ereport(ERROR,
              (errmsg("failed to dlopen '%s': %s", shared_library_path, err)));
    }

    oldcontext = MemoryContextSwitchTo(TopMemoryContext);
    jit_ctx->handles = lappend(jit_ctx->handles, handle);
    MemoryContextSwitchTo(oldcontext);
  }

  /* The current module is compiled. */
  jit_ctx->compiled = true;
  jit_ctx->module_generation = slowjit_module_generation++;
}

static ExprStateEvalFunc slowjit_get_function(SlowJitContext *jit_ctx,
                                              const char *funcname) {
  ListCell *lc;
  ExprStateEvalFunc jitted_func = NULL;

  if (!jit_ctx->compiled) {
    slowjit_compile_module(jit_ctx);
  }

  foreach (lc, jit_ctx->handles) {
    void *handle = (void *)lfirst(lc);

    jitted_func = dlsym(handle, funcname);
    if (jitted_func == NULL) {
      /* Consume the existing error. */
      char *err = dlerror();
      ereport(DEBUG1, (errmsg("cannot find symbol '%s': %s", funcname, err)));
      continue;
    } else {
      return jitted_func;
    }
  }

  ereport(ERROR, (errmsg("failed to jit function '%s'", funcname)));

  return NULL;
}

static Datum slowjit_exec_compiled_expr(ExprState *state, ExprContext *econtext,
                                        bool *isNull) {
  SlowJitCompiledExprState *cstate = state->evalfunc_private;
  ExprStateEvalFunc func;

  CheckExprStillValid(state, econtext);

  func = slowjit_get_function(cstate->jit_ctx, cstate->funcname);
  Assert(func);

  /* remove indirection via this function for future calls */
  state->evalfunc = func;

  return func(state, econtext, isNull);
}

bool slowjit_compile_expr(ExprState *state) {

#define emit_line(...)                                                         \
  do {                                                                         \
    appendStringInfo(&jit_ctx->code_holder, __VA_ARGS__);                      \
    appendStringInfoChar(&jit_ctx->code_holder, '\n');                         \
  } while (0)

#define emit_include(header) emit_line("#include \"%s\"", header)

  PlanState *parent = state->parent;
  SlowJitContext *jit_ctx = NULL;
  char *funcname = NULL;

  /* parent shouldn't be NULL. */
  Assert(parent != NULL);

  /* Initialize the context. */
  if (parent->state->es_jit) {
    jit_ctx = (SlowJitContext *)parent->state->es_jit;
  } else {
    jit_ctx = slowjit_create_context(parent->state->es_jit_flags);
    parent->state->es_jit = &jit_ctx->base;
  }

  /* The code holder is empty, which means this is a new module. */
  if (jit_ctx->code_holder.len == 0) {
    /* We only emit the header once! */
    emit_include("postgres.h");
    emit_include("nodes/execnodes.h");
  }

  /* Emit the jitted function signature. */
  funcname = slowjit_expand_funcname(jit_ctx, "slowjit_eval_expr");
  emit_line("Datum %s(ExprState *state, ExprContext *econtext, bool "
            "*isnull)",
            funcname);

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
      emit_line("  // EEOP_DONE");
      emit_line("  *isnull = state->resnull;");
      emit_line("  return state->resvalue;");

      /* Close function boday. */
      emit_line("}");
      break;
    }
    case EEOP_ASSIGN_TMP: {
      emit_line("  { // EEOP_ASSIGN_TMP");
      emit_line("    int resultnum = %d;", op->d.assign_tmp.resultnum);
      emit_line("    resultslot->tts_values[resultnum] = state->resvalue;");
      emit_line("    resultslot->tts_isnull[resultnum] = state->resnull;");
      emit_line("  }");
      break;
    }
    case EEOP_CONST: {
      emit_line("  { // EEOP_CONST");
      emit_line("    bool *resnull = (bool *) %lu;", (uint64_t)op->resnull);
      emit_line("    Datum *resvalue = (Datum *) %lu;", (uint64_t)op->resvalue);
      emit_line("    *resnull = %d;", op->d.constval.isnull);
      emit_line("    *resvalue = %lu;", op->d.constval.value);
      emit_line("  }");
      break;
    }
    default: {
      ereport(NOTICE, (errmsg("operator: %d is not implemented!", opcode)));
      return false;
    }
    }
  }

  {
    SlowJitCompiledExprState *cstate =
        palloc0(sizeof(SlowJitCompiledExprState));

    cstate->jit_ctx = jit_ctx;
    cstate->funcname = funcname;

    state->evalfunc = slowjit_exec_compiled_expr;
    state->evalfunc_private = cstate;
  }

  return true;
}

void slowjit_release_context(JitContext *jit_ctx) { /* TODO */
}

void slowjit_reset_after_error(void) { /* TODO */
}

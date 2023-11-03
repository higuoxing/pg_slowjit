#include "slowjit.h"

PG_MODULE_MAGIC;

char *slowjit_cc_path = NULL;

void _PG_init(void) {
  DefineCustomStringVariable(
      "slowjit.cc_path",
      "Sets the C compiler to be used on the remote server.", NULL, &slowjit_cc_path,
      "cc", PGC_SU_BACKEND, GUC_EXPLAIN, NULL, NULL, NULL);
}

void _PG_jit_provider_init(JitProviderCallbacks *cb) {
  cb->reset_after_error = slowjit_reset_after_error;
  cb->release_context = slowjit_release_context;
  cb->compile_expr = slowjit_compile_expr;
}

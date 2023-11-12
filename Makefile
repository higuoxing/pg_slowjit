MODULE_big = slowjit
EXTENSION = slowjit

OBJS = slowjit.o

# Disable the bitcode generation.
override with_llvm = no

PG_CONFIG := pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

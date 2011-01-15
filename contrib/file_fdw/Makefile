# contrib/file_fdw/Makefile

MODULE_big = file_fdw
OBJS	= file_fdw.o

DATA_built = file_fdw.sql
DATA = uninstall_file_fdw.sql
REGRESS = file_fdw


ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/file_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

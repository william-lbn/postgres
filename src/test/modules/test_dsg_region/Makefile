# DSGA 普通页 Provider 的真实映射和边界测试。
MODULE_big = test_dsg_region
OBJS = $(WIN32RES) test_dsg_region.o test_dsg_control.o
EXTENSION = test_dsg_region
DATA = test_dsg_region--1.0.sql
REGRESS = test_dsg_region
REGRESS_OPTS = --temp-config $(top_srcdir)/src/test/modules/test_dsg_region/test_dsg_region.conf
ISOLATION = test_dsg_region_shared
ISOLATION_OPTS = --temp-config $(top_srcdir)/src/test/modules/test_dsg_region/test_dsg_region.conf
PGFILEDESC = "test_dsg_region - DSGA region provider tests"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_dsg_region
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
ifeq ($(enable_injection_points),yes)
ISOLATION += test_dsg_buffer_access test_dsg_buffer_grow test_dsg_buffer_fence
ISOLATION += test_dsg_buffer_shrink
EXTRA_INSTALL = src/test/modules/injection_points contrib/pg_buffercache
endif
include $(top_srcdir)/contrib/contrib-global.mk
endif

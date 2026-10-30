MODULE_big = test_dsg_region18
OBJS = $(WIN32RES) test_dsg_region18.o
PGFILEDESC = "test_dsg_region18 - isolated PG18 ordinary-page provider and ABI tests"
EXTENSION = test_dsg_region18
DATA = test_dsg_region18--1.0.sql
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_dsg_region18
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

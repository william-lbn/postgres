#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/buffer
#
# IDENTIFICATION
#    src/backend/storage/buffer/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/buffer
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	buf_init.o \
	buf_table.o \
	bufmgr.o \
	dynamic_shared_buffers.o \
	dsg_buffer_grow.o \
	dsg_buffer_control.o \
	dsg_buffer_shrink.o \
	dsg_work.o \
	dsg_envelope_birth.o \
	freelist.o \
	localbuf.o

include $(top_srcdir)/src/backend/common.mk

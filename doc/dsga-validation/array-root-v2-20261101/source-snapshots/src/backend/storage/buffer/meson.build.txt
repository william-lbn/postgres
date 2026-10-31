# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
  'buf_init.c',
  'buf_table.c',
  'bufmgr.c',
  'dynamic_shared_buffers.c',
  'dsg_buffer_grow.c',
  'dsg_array_root.c',
  'dsg_buffer_control.c',
  'dsg_buffer_shrink.c',
  'dsg_work.c',
  'dsg_envelope_birth.c',
  'dsg_envelope_filter.c',
  'dsg_native_birth_wire.c',
  'dsg_filter_audit.c',
  'freelist.c',
  'localbuf.c',
)

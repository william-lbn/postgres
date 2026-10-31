/* PG18本地core组件：原四数组startup映射与AIO末访问lease；不授resize/normal许可。 */
#ifndef DSG_BUFFER_ARRAYS_H
#define DSG_BUFFER_ARRAYS_H
#include "storage/dsg_region.h"
#define DSG_NATIVE_ARRAY_COUNT 4
#define DSG_NATIVE_ARRAYS_ENV "NEON_DSGA_PG18_NATIVE_ARRAYS"
typedef struct DsgBufferCompletionLease { bool active; bool acquired; } DsgBufferCompletionLease;
typedef struct DsgBufferArraysSnapshot {
 bool enabled;
 int maximum_buffers;
 int startup_buffers;
 DsgRegion ranges[DSG_NATIVE_ARRAY_COUNT];
 Size committed_bytes[DSG_NATIVE_ARRAY_COUNT];
 uint32 active_completions;
 uint32 paused_completions;
 uint64 completed_aio;
 uint64 released_aio_pins;
 bool completion_lock_available;
} DsgBufferArraysSnapshot;
extern bool DsgBufferArraysEnabled(void);
extern Size DsgBufferArraysShmemSize(void);
extern void DsgBufferArraysInit(void);
extern void *DsgBufferArrayAddress(int id);
extern void DsgBufferArraysDetachInherited(void);
extern void DsgBufferArraysSnapshotGet(DsgBufferArraysSnapshot *out);
extern void DsgBufferCompletionEnter(DsgBufferCompletionLease *lease);
extern void DsgBufferCompletionAfterUnpin(void);
extern void DsgBufferCompletionLeave(DsgBufferCompletionLease *lease);
extern void DsgBufferArraysTestPause(bool pause);
extern void DsgBufferArraysTestPauseBeforeUnpin(bool pause);
extern bool DsgBufferArraysTryCompletionBarrier(void);
extern void DsgBufferArraysEndCompletionBarrier(void);
#endif

/*-
 * PG18独立core准备组件。只有显式test-v1:M启用；没有Root出生或生产resize调用。
 * 共享目录/完成lease在原main段，四数组用同版本DsgRegion保留原固定VA。
 * AIO pin/wref清除不等于IOCV/WakePinCountWaiter已结束，完成lease必须覆盖最后访问。
 */
#include "postgres.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/dsg_buffer_arrays.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include <errno.h>
#include <unistd.h>

typedef struct DsgBufferArrayControl {
 LWLock completion_lock;
 pg_atomic_uint32 active_completions;
 pg_atomic_uint32 paused_completions;
 pg_atomic_uint32 test_pause;
 pg_atomic_uint32 test_pause_before_unpin;
 pg_atomic_uint64 completed_aio;
 pg_atomic_uint64 released_aio_pins;
 int maximum;
 int startup;
 Size committed[DSG_NATIVE_ARRAY_COUNT];
} DsgBufferArrayControl;
typedef enum MappingState { ARRAY_EMPTY, ARRAY_SHARED, ARRAY_PARKED, ARRAY_TRANSITION } MappingState;
static struct { DsgRegion region; MappingState state; } mappings[DSG_NATIVE_ARRAY_COUNT];
static DsgBufferArrayControl *array_control;
static bool mode_read;
static bool mode_enabled;
static int maximum_buffers;

bool
DsgBufferArraysEnabled(void)
{
 const char *raw;
 const char *p;
 uint64 value=0;
 if(mode_read) return mode_enabled;
 mode_read=true;
 raw=getenv(DSG_NATIVE_ARRAYS_ENV);
 if(raw==NULL) return false;
#if !defined(__linux__) || defined(EXEC_BACKEND)
 ereport(FATAL,(errmsg("PG18 native arrays require supported Linux fork profile")));
#endif
 if(strncmp(raw,"test-v1:",8)!=0 || raw[8]<'1'||raw[8]>'9')
  ereport(FATAL,(errmsg("invalid explicit PG18 native arrays test profile")));
 if(raw[8]=='0')ereport(FATAL,(errmsg("invalid PG18 native arrays maximum")));
 for(p=raw+8;*p;p++) {
  if(*p<'0'||*p>'9'||value>((uint64)INT_MAX-(*p-'0'))/10)
   ereport(FATAL,(errmsg("invalid PG18 native arrays maximum")));
  value=value*10+(*p-'0');
 }
 if(value<(uint64)NBuffers || value==0)
  ereport(FATAL,(errmsg("PG18 native arrays maximum is below startup shared_buffers")));
 maximum_buffers=(int)value;
 enable_dynamic_shared_buffers=true;max_shared_buffers=maximum_buffers;
 mode_enabled=true;return true;
}
Size
DsgBufferArraysShmemSize(void)
{
 return DsgBufferArraysEnabled()?add_size(MAXALIGN(sizeof(DsgBufferArrayControl)),DynamicSharedBuffersShmemSize()):0;
}
static void
ClearAliases(void)
{
 DSBCtrl=NULL;array_control=NULL;BufferDescriptors=NULL;BufferBlocks=NULL;BufferIOCVArray=NULL;CkptBufferIds=NULL;
}
static void
DestroyArrays(int code,Datum arg)
{
 int i;
 /* 原18PM只在全部共享子类（含IO worker）退出后重建。子进程exit回调由PG重置。 */
 for(i=0;i<DSG_NATIVE_ARRAY_COUNT;i++) {
  if(mappings[i].state==ARRAY_TRANSITION) elog(FATAL,"PG18 array ownership unknown");
  if(mappings[i].state!=ARRAY_SHARED) continue;
  mappings[i].state=ARRAY_TRANSITION;
  DsgRegionPark(&mappings[i].region);
  mappings[i].state=ARRAY_PARKED;
 }
 ClearAliases();
}
void
DsgBufferArraysDetachInherited(void)
{
 int i;
 if(!mode_enabled)return;
 /* 只改进程私有所有权表，不MemSet共享目录。非共享logger等不保留旧backing。 */
 for(i=0;i<DSG_NATIVE_ARRAY_COUNT;i++) {
  if(mappings[i].state==ARRAY_TRANSITION)elog(FATAL,"PG18 child inherited unknown array");
  DsgRegionDestroy(&mappings[i].region);mappings[i].state=ARRAY_EMPTY;
 }
 ClearAliases();
}
void
DsgBufferArraysInit(void)
{
 bool found;
 int i;
 const Size elements[DSG_NATIVE_ARRAY_COUNT]={sizeof(BufferDescPadded),BLCKSZ,sizeof(ConditionVariableMinimallyPadded),sizeof(CkptSortItem)};
 if(!DsgBufferArraysEnabled())return;
 DynamicSharedBuffersInit();
 array_control=ShmemInitStruct("DSGA PG18 native buffer arrays",sizeof(*array_control),&found);
 if(found)elog(FATAL,"PG18 native arrays cannot attach unowned shared directory");
 MemSet(array_control,0,sizeof(*array_control));
 {int tranche=LWLockNewTrancheId();LWLockRegisterTranche(tranche,"DSGA native completion");LWLockInitialize(&array_control->completion_lock,tranche);}
 pg_atomic_init_u32(&array_control->active_completions,0);
 pg_atomic_init_u32(&array_control->paused_completions,0);
 pg_atomic_init_u32(&array_control->test_pause,0);
 pg_atomic_init_u32(&array_control->test_pause_before_unpin,0);
 pg_atomic_init_u64(&array_control->completed_aio,0);
 pg_atomic_init_u64(&array_control->released_aio_pins,0);
 array_control->maximum=maximum_buffers;array_control->startup=NBuffers;
 on_shmem_exit(DestroyArrays,(Datum)0);
 for(i=0;i<DSG_NATIVE_ARRAY_COUNT;i++) {
  Size maximum=mul_size(maximum_buffers,elements[i]);
  Size initial=mul_size(NBuffers,elements[i]);
  DsgMemoryResult result;
  if(mappings[i].state==ARRAY_EMPTY)DsgRegionReserve(&mappings[i].region,maximum);
  else {
   if(mappings[i].state!=ARRAY_PARKED||mappings[i].region.maximum_bytes!=maximum)elog(FATAL,"PG18 array geometry or lifetime changed");
   mappings[i].state=ARRAY_TRANSITION;DsgRegionRearm(&mappings[i].region);
  }
  mappings[i].state=ARRAY_SHARED;
  result=DsgRegionPopulate(&mappings[i].region,0,initial);
  if(result.status!=DSG_MEMORY_OK) {errno=result.system_errno;ereport(FATAL,(errcode(ERRCODE_OUT_OF_MEMORY),errmsg("could not populate PG18 native array: %m")));}
  array_control->committed[i]=TYPEALIGN(mappings[i].region.page_bytes,initial);
  DSBCtrl->arrays[i].region=mappings[i].region;
  DSBCtrl->arrays[i].element_bytes=elements[i];
  DSBCtrl->arrays[i].committed_upper_bytes=array_control->committed[i];
 }
 if((uintptr_t)mappings[1].region.address%PG_IO_ALIGN_SIZE!=0)elog(FATAL,"PG18 array I/O alignment unsupported");
}
void *
DsgBufferArrayAddress(int id)
{
 if(!array_control||id<0||id>=DSG_NATIVE_ARRAY_COUNT||mappings[id].state!=ARRAY_SHARED)elog(FATAL,"PG18 native array not initialized");
 return mappings[id].region.address;
}
static void
DsgCompletionTestWait(pg_atomic_uint32 *flag)
{
 instr_time started;
 instr_time elapsed;
 /* 只供显式test profile的真实worker窗口测试；不注册扩展/生产GUC或授权路径。
  * 等待无PG分配/ERROR，单调时钟采用固定5秒截止点；调度延迟不重置截止。 */
 if(pg_atomic_read_u32(flag)) {
  pg_atomic_fetch_add_u32(&array_control->paused_completions,1);
  INSTR_TIME_SET_CURRENT(started);
  while(pg_atomic_read_u32(flag)){
   INSTR_TIME_SET_CURRENT(elapsed);INSTR_TIME_SUBTRACT(elapsed,started);
   if(INSTR_TIME_GET_MILLISEC(elapsed)>=5000)break;
   pg_usleep(1000L);
  }
  pg_atomic_fetch_sub_u32(&array_control->paused_completions,1);
 }
}
void
DsgBufferCompletionEnter(DsgBufferCompletionLease *lease)
{
 lease->active=false;lease->acquired=false;
 if(!array_control)return;
 /* 先持lease，后释放原AIO pin。后继shrinker只可conditional独占；不可持独占等IO。 */
 if(!LWLockHeldByMe(&array_control->completion_lock)) {
  LWLockAcquire(&array_control->completion_lock,LW_SHARED);lease->acquired=true;
 }
 pg_atomic_fetch_add_u32(&array_control->active_completions,1);lease->active=true;
 /* TEST_ONLY真实停点位于原IO pin/wref清除前；只延迟既有completion，
  * 不伪造BM_IO/refcount，也不替代shrinker真实排水判断。默认原子值0。 */
 DsgCompletionTestWait(&array_control->test_pause_before_unpin);
}
void
DsgBufferCompletionAfterUnpin(void)
{
 if(!array_control)return;
 pg_atomic_fetch_add_u64(&array_control->released_aio_pins,1);
 DsgCompletionTestWait(&array_control->test_pause);
}
void
DsgBufferCompletionLeave(DsgBufferCompletionLease *lease)
{
 if(!lease->active)return;
 pg_atomic_fetch_add_u64(&array_control->completed_aio,1);
 Assert(pg_atomic_read_u32(&array_control->active_completions)>0);
 pg_atomic_fetch_sub_u32(&array_control->active_completions,1);
 if(lease->acquired)LWLockRelease(&array_control->completion_lock);
 lease->active=false;lease->acquired=false;
}
void
DsgBufferArraysSnapshotGet(DsgBufferArraysSnapshot *out)
{
 int i;
 MemSet(out,0,sizeof(*out));
 if(!array_control)return;
 out->enabled=true;out->maximum_buffers=array_control->maximum;out->startup_buffers=array_control->startup;
 for(i=0;i<DSG_NATIVE_ARRAY_COUNT;i++){out->ranges[i]=mappings[i].region;out->committed_bytes[i]=array_control->committed[i];}
 if(DSBCtrl){DsgBufferSnapshot native;DsgBufferGetSnapshot(&native);for(i=0;i<DSG_NATIVE_ARRAY_COUNT;i++)out->committed_bytes[i]=native.committed_upper_bytes[i];}
 out->active_completions=pg_atomic_read_u32(&array_control->active_completions);
 out->paused_completions=pg_atomic_read_u32(&array_control->paused_completions);
 out->completed_aio=pg_atomic_read_u64(&array_control->completed_aio);
 out->released_aio_pins=pg_atomic_read_u64(&array_control->released_aio_pins);
 /* 成功只表示该瞬间没有completion lease；未覆盖其他pin/readers，绝不准许释放。 */
 if(!LWLockHeldByMe(&array_control->completion_lock)&&LWLockConditionalAcquire(&array_control->completion_lock,LW_EXCLUSIVE)){
  out->completion_lock_available=true;LWLockRelease(&array_control->completion_lock);
 }
}
void
DsgBufferArraysTestPause(bool pause)
{
 if(!array_control)ereport(ERROR,(errmsg("explicit PG18 arrays test profile required")));
 pg_atomic_write_u32(&array_control->test_pause,pause?1:0);
}

/* 持accessX时只能try；失败不等待IO/pin。高水位发布后尾部无新pin，才可放该短屏障。 */
bool
DsgBufferArraysTryCompletionBarrier(void)
{
 if(!array_control || LWLockHeldByMe(&array_control->completion_lock))return false;
 if(!LWLockConditionalAcquire(&array_control->completion_lock,LW_EXCLUSIVE))return false;
 if(pg_atomic_read_u32(&array_control->active_completions)!=0){LWLockRelease(&array_control->completion_lock);return false;}
 return true;
}
void
DsgBufferArraysEndCompletionBarrier(void)
{
 Assert(array_control && LWLockHeldByMeInMode(&array_control->completion_lock,LW_EXCLUSIVE));
 LWLockRelease(&array_control->completion_lock);
}

void
DsgBufferArraysTestPauseBeforeUnpin(bool pause)
{
 if(!array_control)ereport(ERROR,(errmsg("explicit PG18 arrays test profile required")));
 pg_atomic_write_u32(&array_control->test_pause_before_unpin,pause?1:0);
}

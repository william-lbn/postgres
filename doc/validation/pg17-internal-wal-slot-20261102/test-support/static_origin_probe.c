#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/bgworker_internals.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/wait_event.h"
#include "utils/acl.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
PG_MODULE_MAGIC;
void _PG_init(void);
PGDLLEXPORT void WalProposerMain(Datum main_arg);
Datum dsg_static_probe_register(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(dsg_static_probe_register);
static const char *outdir(void) {
 const char *p=getenv("DSG_STATIC_SLOT_TEST_OUT");
 if(p==NULL || strcmp(p,"/evidence")!=0) elog(ERROR,"test-only fixed output directory missing");
 return p;
}
static void write_fact(const char *file,const char *value) {
 char path[MAXPGPATH];int fd;size_t n=strlen(value);
 snprintf(path,sizeof(path),"%s/%s",outdir(),file);
 fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
 if(fd<0) elog(ERROR,"test-only fact create failed");
 if(write(fd,value,n)!=(ssize_t)n){close(fd);elog(ERROR,"test-only fact write failed");}
 close(fd);
}
static void descriptor(BackgroundWorker *w) {
 memset(w,0,sizeof(*w));w->bgw_flags=BGWORKER_SHMEM_ACCESS;
 w->bgw_start_time=BgWorkerStart_RecoveryFinished;
 w->bgw_restart_time=BGW_NEVER_RESTART;
 strlcpy(w->bgw_library_name,"neon",BGW_MAXLEN);
 strlcpy(w->bgw_function_name,"WalProposerMain",BGW_MAXLEN);
 strlcpy(w->bgw_name,"WAL proposer",BGW_MAXLEN);
 strlcpy(w->bgw_type,"WAL proposer",BGW_MAXLEN);
 w->bgw_main_arg=(Datum)0;w->bgw_notify_pid=0;
}
void _PG_init(void) {
 BackgroundWorker w;
 if(!process_shared_preload_libraries_in_progress)return;
 descriptor(&w);RegisterBackgroundWorker(&w);
}
void WalProposerMain(Datum main_arg) {
 char file[80],data[240],release[MAXPGPATH];bool value;
 (void)main_arg;InitProcessPhase2();value=BackgroundWorkerIsStaticWalProposer();
 snprintf(file,sizeof(file),"origin-%d.json",MyProcPid);
 snprintf(data,sizeof(data),"{\"pid\":%d,\"getter\":%s,\"database_invalid\":%s,\"under_postmaster\":%s}\n",MyProcPid,value?"true":"false",MyDatabaseId==InvalidOid?"true":"false",IsUnderPostmaster?"true":"false");
 write_fact(file,data);snprintf(release,sizeof(release),"%s/release-%d",outdir(),MyProcPid);
 for(int i=0;i<1000;i++) {
  if(access(release,F_OK)==0)return;
  CHECK_FOR_INTERRUPTS();(void)WaitLatch(MyLatch,WL_LATCH_SET|WL_TIMEOUT|WL_EXIT_ON_PM_DEATH,10L,PG_WAIT_EXTENSION);ResetLatch(MyLatch);
 }
 elog(ERROR,"test-only worker release deadline exceeded");
}
Datum dsg_static_probe_register(PG_FUNCTION_ARGS) {
 BackgroundWorker w;BackgroundWorkerHandle *h=NULL;pid_t pid=0;BgwHandleStatus status;char value[80];bool forged=PG_GETARG_BOOL(0);
 if(!superuser())ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("test-only superuser helper")));
 descriptor(&w);if(forged)w.bgw_flags|=BGWORKER_DSG_STATIC_WAL_PROPOSER;
 if(!RegisterDynamicBackgroundWorker(&w,&h))PG_RETURN_INT64(-1);
 for(int i=0;i<500;i++) {
  status=GetBackgroundWorkerPid(h,&pid);
  if(status==BGWH_STARTED)break;
  if(status==BGWH_STOPPED || status==BGWH_POSTMASTER_DIED)elog(ERROR,"test-only dynamic child did not start");
  CHECK_FOR_INTERRUPTS();(void)WaitLatch(MyLatch,WL_LATCH_SET|WL_TIMEOUT|WL_EXIT_ON_PM_DEATH,10L,PG_WAIT_EXTENSION);ResetLatch(MyLatch);
 }
 if(pid<=0)elog(ERROR,"test-only actual child PID unavailable");
 snprintf(value,sizeof(value),"{\"pid\":%d}\n",pid);write_fact("dynamic-parent.json",value);
 for(int i=0;i<1000;i++) {
  status=GetBackgroundWorkerPid(h,&pid);
  if(status==BGWH_STOPPED)PG_RETURN_INT64((int64)1);
  if(status==BGWH_POSTMASTER_DIED)elog(ERROR,"test-only postmaster died");
  CHECK_FOR_INTERRUPTS();(void)WaitLatch(MyLatch,WL_LATCH_SET|WL_TIMEOUT|WL_EXIT_ON_PM_DEATH,10L,PG_WAIT_EXTENSION);ResetLatch(MyLatch);
 }
 elog(ERROR,"test-only dynamic shutdown deadline exceeded");PG_RETURN_NULL();
}

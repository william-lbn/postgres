/* DSM1仅测试实际OS/控制表生命周期，不签Source/normal。 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "common/pg_prng.h"
#include "lib/stringinfo.h"
#include "storage/dsg_dsm.h"
#include "storage/dsm.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/pgstat_internal.h"
#define MAGIC18 UINT64CONST(0x183dd5a123456789)
PG_FUNCTION_INFO_V1(test_dsg_dsm18_snapshot);
PG_FUNCTION_INFO_V1(test_dsg_dsm18_create);
PG_FUNCTION_INFO_V1(test_dsg_dsm18_touch);
PG_FUNCTION_INFO_V1(test_dsg_dsm18_collision);
static void Admin(void)
{
 if(!superuser() || !superuser_arg(GetSessionUserId()))
  ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("PG18 DSM tests require original administrator")));
}
Datum
test_dsg_dsm18_snapshot(PG_FUNCTION_ARGS)
{
 DsgDsmSnapshot s;DsgConnectionSnapshot *c=&s.prepared.connections;DsgDsmCounters *d=&s.dsm;StringInfoData out;
 Admin();DsgDsmGetSnapshot(&s);initStringInfo(&out);
 appendStringInfo(&out,
 "{\"api\":1,\"prepared_api\":1,\"connection_api\":1,\"work_api\":2,\"boot\":\"" UINT64_FORMAT "\","
 "\"admission_open\":%s,\"prepared_slots\":%u,\"requested_serial\":%u,\"applied_serial\":%u,\"requested_open\":%s,\"applied_open\":%s,\"acknowledged\":%s,\"public_connections_blocked\":%s,"
 "\"reserved_upper_bytes\":\"" UINT64_FORMAT "\",\"control_upper_bytes\":\"" UINT64_FORMAT "\",\"external_budget_bytes\":\"" UINT64_FORMAT "\","
 "\"external_segments\":%u,\"pending_creations\":%u,\"unknown_segments\":%u,\"page_bytes\":%u,\"limit_enforced\":%s,\"accounting_valid\":%s,"
 "\"snapshot_bytes\":%zu,\"prepared_snapshot_bytes\":%zu,\"normal_permit\":false,\"Source\":false,\"memory_context_budget\":false}",
 c->work.observed.boot_id,c->work.observed.admission_open?"true":"false",s.prepared.prepared_slots,c->requested_serial,c->applied_serial,c->requested_open?"true":"false",c->applied_open?"true":"false",c->acknowledged?"true":"false",c->public_connections_blocked?"true":"false",
 d->reserved_upper_bytes,d->control_upper_bytes,d->external_budget_bytes,d->external_segments,d->pending_creations,d->unknown_segments,d->page_bytes,d->limit_enforced?"true":"false",d->accounting_valid?"true":"false",sizeof(s),sizeof(s.prepared));
 PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
Datum
test_dsg_dsm18_create(PG_FUNCTION_ARGS)
{
 int64 bytes=PG_GETARG_INT64(0);bool pin=PG_GETARG_BOOL(1);dsm_segment *seg;dsm_handle h;Size mapped;StringInfoData out;
 Admin();if(bytes<8 || bytes>1048576)elog(ERROR,"DSM test fixture request outside bounded range");
 seg=dsm_create((Size)bytes,0);*((uint64*)dsm_segment_address(seg))=MAGIC18;h=dsm_segment_handle(seg);mapped=dsm_segment_map_length(seg);
 if(pin)dsm_pin_segment(seg);dsm_detach(seg);initStringInfo(&out);appendStringInfo(&out,"{\"handle\":%u,\"mapped_bytes\":%zu,\"pinned\":%s,\"normal_permit\":false}",h,mapped,pin?"true":"false");PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
Datum
test_dsg_dsm18_touch(PG_FUNCTION_ARGS)
{
 dsm_handle h=(dsm_handle)PG_GETARG_INT64(0);bool release=PG_GETARG_BOOL(1);dsm_segment *seg;Size mapped;StringInfoData out;
 Admin();seg=dsm_attach(h);if(seg==NULL)elog(ERROR,"DSM test actual segment cannot be attached");
 INJECTION_POINT("test-dsg-dsm18-after-attach",NULL);
 if(*((uint64*)dsm_segment_address(seg))!=MAGIC18)elog(ERROR,"DSM test actual payload differs");mapped=dsm_segment_map_length(seg);
 if(release)dsm_unpin_segment(h);dsm_detach(seg);initStringInfo(&out);appendStringInfo(&out,"{\"handle\":%u,\"mapped_bytes\":%zu,\"actual_payload_valid\":true,\"released\":%s}",h,mapped,release?"true":"false");PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
/* 精确自有OS碰撞fixture，原对象不注册/不算产品预算；不作Source资格。 */
Datum
test_dsg_dsm18_collision(PG_FUNCTION_ARGS)
{
 typedef struct {bool created;void *priv;void *addr;Size mapped;} Fixture;
 Fixture *fixture;pg_prng_state saved;dsm_handle candidate;dsm_segment *volatile seg=NULL;StringInfoData out;dsm_handle retried=0;
 Admin();fixture=palloc0(sizeof(Fixture));saved=pg_global_prng_state;pg_prng_seed(&pg_global_prng_state,183123);candidate=pg_prng_uint32(&pg_global_prng_state)<<1;if(candidate==DSM_HANDLE_INVALID)elog(ERROR,"DSM collision fixture invalid candidate");
 PG_TRY();
 {
  fixture->created=dsm_impl_op(DSM_OP_CREATE,candidate,8192,&fixture->priv,&fixture->addr,&fixture->mapped,ERROR);if(!fixture->created)elog(ERROR,"DSM collision fixture candidate already used");*((uint64*)fixture->addr)=MAGIC18;
  pg_prng_seed(&pg_global_prng_state,183123);seg=dsm_create(8192,0);retried=dsm_segment_handle(seg);if(retried==candidate || *((uint64*)fixture->addr)!=MAGIC18)elog(ERROR,"DSM collision corrupted original object");dsm_detach(seg);seg=NULL;
  if(!dsm_impl_op(DSM_OP_DESTROY,candidate,0,&fixture->priv,&fixture->addr,&fixture->mapped,ERROR))elog(ERROR,"DSM collision fixture exact cleanup failed");fixture->created=false;pg_global_prng_state=saved;
 }
 PG_CATCH();
 {
  pg_global_prng_state=saved;if(fixture->created)dsm_impl_op(DSM_OP_DESTROY,candidate,0,&fixture->priv,&fixture->addr,&fixture->mapped,WARNING);PG_RE_THROW();
 }
 PG_END_TRY();
 initStringInfo(&out);appendStringInfo(&out,"{\"original_handle\":%u,\"retried_handle\":%u,\"original_actual_payload_preserved\":true,\"exact_fixture_destroyed\":true,\"Source\":false,\"normal_permit\":false}",candidate,retried);PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/* 原生stats DSA直接取证，不以猜测分配来源放宽测试总cap。 */
PG_FUNCTION_INFO_V1(test_dsg_dsm18_pgstat_area);
Datum
test_dsg_dsm18_pgstat_area(PG_FUNCTION_ARGS)
{
 Admin();if(pgStatLocal.dsa==NULL)elog(ERROR,"actual pgstat DSA is unavailable");
 fprintf(stderr,"PG18_DSM_TEST_PGSTAT_AREA_BEGIN pid=%d\n",MyProcPid);
 dsa_dump(pgStatLocal.dsa);
 fprintf(stderr,"PG18_DSM_TEST_PGSTAT_AREA_END pid=%d\n",MyProcPid);
 PG_RETURN_VOID();
}

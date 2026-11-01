/* PG18 Work2直接原生测试；fixture setter不认证预算或normal许可。 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/dsg_work.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(test_dsg_work18_snapshot);
PG_FUNCTION_INFO_V1(test_dsg_work18_admission);

static void
WorkAdmin(void)
{
 if(!superuser() || !superuser_arg(GetSessionUserId()))
  ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("PG18 Work tests require original administrator")));
}

Datum
test_dsg_work18_snapshot(PG_FUNCTION_ARGS)
{
 DsgWorkSnapshot s;
 StringInfoData out;
 WorkAdmin();DsgWorkGetSnapshot(&s);initStringInfo(&out);
 appendStringInfo(&out,
 "{\"api\":2,\"boot\":\"" UINT64_FORMAT "\",\"generation\":\"" UINT64_FORMAT "\","
 "\"admission_revision\":\"" UINT64_FORMAT "\",\"admission_open\":%s,"
 "\"client_processes\":%u,\"startup_clients\":%u,\"idle_clients\":%u,\"frontend_reads\":%u,"
 "\"frontend_executes\":%u,\"bounded_inputs\":%u,\"retained_transactions\":%u,"
 "\"background_processes\":%u,\"auxiliary_processes\":%u,\"observer_excluded\":%s,"
 "\"foreground_idle\":%s,\"snapshot_bytes\":%zu,\"normal_permit\":false,\"memory_context_budget\":false}",
 s.observed.boot_id,s.observed.generation,s.observed.admission_revision,s.observed.admission_open?"true":"false",
 s.client_processes,s.startup_clients,s.idle_clients,s.frontend_reads,s.frontend_executes,s.bounded_inputs,s.retained_transactions,
 s.background_processes,s.auxiliary_processes,s.observer_excluded?"true":"false",s.foreground_idle?"true":"false",sizeof(s));
 PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

Datum
test_dsg_work18_admission(PG_FUNCTION_ARGS)
{
 DsgBufferSnapshot s;
 DsgBufferIssuerFenceResponse fence;
 DsgBufferAdmissionRequest request={0};
 DsgBufferAdmissionResponse response;
 WorkAdmin();DsgBufferGetSnapshot(&s);
 if(s.issuer_floor_epoch==0){DsgBufferFenceIssuer(s.boot_id,1,&fence);if(fence.disposition!=DSG_ISSUER_FENCED)elog(ERROR,"PG18 Work test issuer fence not complete");DsgBufferGetSnapshot(&s);}
 if(s.admission_revision==PG_UINT64_MAX)elog(ERROR,"PG18 Work test revision exhausted");
 request.boot_id=s.boot_id;request.issuer_epoch=s.issuer_floor_epoch;request.expected_generation=s.generation;
 request.revision=s.admission_revision+1;request.open=PG_GETARG_BOOL(0);MemSet(request.context_sha256,0x18,sizeof(request.context_sha256));
 DsgBufferControlAdmission(&request,&response);
 if(response.disposition!=DSG_ADMISSION_APPLIED)elog(ERROR,"PG18 Work test admission rejected disposition=%d",response.disposition);
 PG_RETURN_VOID();
}

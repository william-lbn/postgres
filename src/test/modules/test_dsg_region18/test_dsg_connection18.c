/* PG18 Connection1直接测试；只观察原PM门禁，不认证预算/normal许可。 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/dsg_work.h"
#include "utils/builtins.h"
PG_FUNCTION_INFO_V1(test_dsg_connection18_snapshot);
Datum
test_dsg_connection18_snapshot(PG_FUNCTION_ARGS)
{
 DsgConnectionSnapshot s;
 StringInfoData out;
 if(!superuser() || !superuser_arg(GetSessionUserId()))
  ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("PG18 Connection tests require original administrator")));
 DsgConnectionGetSnapshot(&s);initStringInfo(&out);
 appendStringInfo(&out,
 "{\"api\":1,\"work_api\":2,\"boot\":\"" UINT64_FORMAT "\",\"generation\":\"" UINT64_FORMAT "\","
 "\"admission_revision\":\"" UINT64_FORMAT "\",\"admission_open\":%s,\"enabled\":%s,"
 "\"requested_serial\":%u,\"applied_serial\":%u,\"requested_open\":%s,\"applied_open\":%s,"
 "\"acknowledged\":%s,\"private_management_ready\":%s,\"public_connections_blocked\":%s,"
 "\"client_processes\":%u,\"startup_clients\":%u,\"idle_clients\":%u,\"frontend_reads\":%u,"
 "\"frontend_executes\":%u,\"bounded_inputs\":%u,\"retained_transactions\":%u,"
 "\"observer_excluded\":%s,\"foreground_idle\":%s,\"snapshot_bytes\":%zu,"
 "\"normal_permit\":false,\"memory_context_budget\":false}",
 s.work.observed.boot_id,s.work.observed.generation,s.work.observed.admission_revision,s.work.observed.admission_open?"true":"false",s.enabled?"true":"false",
 s.requested_serial,s.applied_serial,s.requested_open?"true":"false",s.applied_open?"true":"false",s.acknowledged?"true":"false",s.private_management_ready?"true":"false",s.public_connections_blocked?"true":"false",
 s.work.client_processes,s.work.startup_clients,s.work.idle_clients,s.work.frontend_reads,s.work.frontend_executes,s.work.bounded_inputs,s.work.retained_transactions,
 s.work.observer_excluded?"true":"false",s.work.foreground_idle?"true":"false",sizeof(s));
 PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

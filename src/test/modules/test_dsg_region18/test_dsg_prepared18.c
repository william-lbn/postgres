/* Prepared1测试只观察原两阶段资源，不授normal或预算资格。 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/dsg_work.h"
#include "utils/builtins.h"
PG_FUNCTION_INFO_V1(test_dsg_prepared18_snapshot);
Datum
test_dsg_prepared18_snapshot(PG_FUNCTION_ARGS)
{
 DsgPreparedSnapshot s;
 DsgConnectionSnapshot *c=&s.connections;
 DsgWorkSnapshot *w=&c->work;
 StringInfoData out;
 if(!superuser() || !superuser_arg(GetSessionUserId()))
  ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("PG18 Prepared tests require original administrator")));
 DsgPreparedGetSnapshot(&s);initStringInfo(&out);
 appendStringInfo(&out,
 "{\"api\":1,\"connection_api\":1,\"work_api\":2,\"boot\":\"" UINT64_FORMAT "\",\"admission_open\":%s,"
 "\"prepared_slots\":%u,\"maximum_prepared_slots\":%u,\"enabled\":%s,\"requested_serial\":%u,\"applied_serial\":%u,"
 "\"requested_open\":%s,\"applied_open\":%s,\"acknowledged\":%s,\"public_connections_blocked\":%s,\"private_management_ready\":%s,"
 "\"client_processes\":%u,\"startup_clients\":%u,\"frontend_executes\":%u,\"foreground_idle\":%s,\"observer_excluded\":%s,"
 "\"snapshot_bytes\":%zu,\"connection_snapshot_bytes\":%zu,\"normal_permit\":false,\"memory_context_budget\":false}",
 w->observed.boot_id,w->observed.admission_open?"true":"false",s.prepared_slots,s.maximum_prepared_slots,c->enabled?"true":"false",c->requested_serial,c->applied_serial,
 c->requested_open?"true":"false",c->applied_open?"true":"false",c->acknowledged?"true":"false",c->public_connections_blocked?"true":"false",c->private_management_ready?"true":"false",
 w->client_processes,w->startup_clients,w->frontend_executes,w->foreground_idle?"true":"false",w->observer_excluded?"true":"false",sizeof(s),sizeof(*c));
 PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

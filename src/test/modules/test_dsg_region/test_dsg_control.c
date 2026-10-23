/*-------------------------------------------------------------------------
 * test_dsg_control.c -- 生产内核控制入口的真实SQL测试桥。
 *
 * 仅转换参数/输出，不修改共享状态、pin、IO或Provider回执。context_token
 * 只用于验证不同外层意图的绑定，不是生产SHA/认证。所有调用必须为管理员；
 * 测试即使显式GRANT或套SECURITY DEFINER也不能越过会话身份检查。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/dynamic_shared_buffers.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/injection_point.h"

PG_FUNCTION_INFO_V1(test_dsg_control);
PG_FUNCTION_INFO_V1(test_dsg_control_parse);
PG_FUNCTION_INFO_V1(test_dsg_control_hold_guard);
PG_FUNCTION_INFO_V1(test_dsg_fence_issuer);
PG_FUNCTION_INFO_V1(test_dsg_control_admission);

static void
DsgTestControlCheck(void)
{
	if (!superuser() || !superuser_arg(GetSessionUserId()))
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("DSGA control test requires a superuser session")));
}

static uint64
DsgTestControlUint64(text *input)
{
	uint64		value;

	if (!DsgBufferParseUint64(VARDATA_ANY(input), VARSIZE_ANY_EXHDR(input), &value))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("DSGA uint64 must be a canonical decimal string")));
	return value;
}

/*- 仅翻译参数和真实快照；token生成定长测试上下文，不代表生产SHA或预算证明。 */
Datum
test_dsg_control_admission(PG_FUNCTION_ARGS)
{
	DsgBufferAdmissionRequest request;
	DsgBufferAdmissionResponse response;
	DsgBufferAccess access;
	StringInfoData output;
	int32		token;
	bool		guarded;

	DsgTestControlCheck();
	MemSet(&request, 0, sizeof(request));
	request.boot_id = DsgTestControlUint64(PG_GETARG_TEXT_PP(0));
	request.issuer_epoch = DsgTestControlUint64(PG_GETARG_TEXT_PP(1));
	request.revision = DsgTestControlUint64(PG_GETARG_TEXT_PP(2));
	request.expected_generation = DsgTestControlUint64(PG_GETARG_TEXT_PP(3));
	request.open = PG_GETARG_BOOL(4);
	token = PG_GETARG_INT32(5);
	memcpy(request.context_sha256, &token, sizeof(token));
	guarded = PG_GETARG_BOOL(6);
	if (guarded)
		access = DsgBufferAccessBegin();
	DsgBufferControlAdmission(&request, &response);
	if (guarded)
		DsgBufferAccessEnd(&access);
	initStringInfo(&output);
	appendStringInfo(&output,
					 "{\"disposition\":%d,\"admission_open\":%s,\"admission_revision\":\"" UINT64_FORMAT "\"}",
					 response.disposition, response.observed.admission_open ? "true" : "false", response.observed.admission_revision);
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(output.data)));
}

static void
DsgTestControlSnapshot(StringInfo output, const DsgBufferSnapshot *snapshot)
{
	const DsgBufferResizeRequest *request = &snapshot->operation.request;
	int			i;

	/*
	 * 所有uint64/Size均为字符串，避免JSON number跨语言丢失高位身份。
	 */
	appendStringInfo(output,
					 "{\"boot\":\"" UINT64_FORMAT "\",\"epoch\":\"" UINT64_FORMAT
					 "\",\"revision\":\"" UINT64_FORMAT "\",\"hi\":\"" UINT64_FORMAT
					 "\",\"lo\":\"" UINT64_FORMAT "\",\"generation\":\"" UINT64_FORMAT
					 "\",\"issuer_floor\":\"" UINT64_FORMAT
					 "\",\"low\":%d,\"high\":%d,\"phase\":%d,\"reason\":%d,\"kind\":%d,"
					 "\"cancel_requested\":%s,\"cancel_closed\":%s,\"replayed\":%s,\"arrays\":[",
					 snapshot->boot_id, request->issuer_epoch, request->revision,
					 request->operation_id_high, request->operation_id_low, snapshot->generation,
					 snapshot->issuer_floor_epoch,
					 snapshot->low, snapshot->high, snapshot->operation.phase, snapshot->operation.reason,
					 snapshot->operation.kind, snapshot->cancel_requested ? "true" : "false",
					 snapshot->cancel_closed ? "true" : "false", snapshot->replayed ? "true" : "false");
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		if (i != 0)
			appendStringInfoChar(output, ',');
		appendStringInfo(output,
						 "{\"upper\":\"%zu\",\"unknown\":%s,\"state\":%d,"
						 "\"epoch\":\"" UINT64_FORMAT "\",\"revision\":\"" UINT64_FORMAT
						 "\",\"hi\":\"" UINT64_FORMAT "\",\"lo\":\"" UINT64_FORMAT
						 "\",\"generation\":\"" UINT64_FORMAT "\"}",
						 snapshot->committed_upper_bytes[i], snapshot->allocation_unknown[i] ? "true" : "false",
						 snapshot->release_state[i], snapshot->release_issuer_epoch[i],
						 snapshot->release_revision[i], snapshot->release_operation_id_high[i],
						 snapshot->release_operation_id_low[i], snapshot->release_generation[i]);
	}
	appendStringInfoString(output, "]}");
}

Datum
test_dsg_control_parse(PG_FUNCTION_ARGS)
{
	uint64		value;

	DsgTestControlCheck();
	value = DsgTestControlUint64(PG_GETARG_TEXT_PP(0));
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(UINT64_FORMAT, value)));
}

/* 只转换真实内核接管事实，不改锁/状态；guarded负例必须触发原生锁序拒绝。 */
Datum
test_dsg_fence_issuer(PG_FUNCTION_ARGS)
{
	DsgBufferIssuerFenceResponse response;
	DsgBufferAccess access;
	StringInfoData output;
	uint64		boot;
	uint64		epoch;
	bool		guarded;

	DsgTestControlCheck();
	boot = DsgTestControlUint64(PG_GETARG_TEXT_PP(0));
	epoch = DsgTestControlUint64(PG_GETARG_TEXT_PP(1));
	guarded = PG_GETARG_BOOL(2);
	if (guarded)
		access = DsgBufferAccessBegin();
	DsgBufferFenceIssuer(boot, epoch, &response);
	if (guarded)
		DsgBufferAccessEnd(&access);
	initStringInfo(&output);
	appendStringInfo(&output, "{\"requested_boot\":\"" UINT64_FORMAT
					 "\",\"requested_epoch\":\"" UINT64_FORMAT "\",\"disposition\":%d,\"observed\":",
					 response.requested_boot_id, response.requested_epoch, response.disposition);
	DsgTestControlSnapshot(&output, &response.observed);
	appendStringInfoChar(&output, '}');
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(output.data)));
}

/* 真实读侧Guard阻挡发布；注入点只暂停，不伪造LWLock或占用状态。 */
Datum
test_dsg_control_hold_guard(PG_FUNCTION_ARGS)
{
	DsgBufferAccess access;

	DsgTestControlCheck();
	access = DsgBufferAccessBegin();
	INJECTION_POINT("dsg-test-control-guard-held");
	DsgBufferAccessEnd(&access);
	PG_RETURN_BOOL(true);
}

Datum
test_dsg_control(PG_FUNCTION_ARGS)
{
	char	   *action;
	DsgBufferResizeRequest request;
	DsgBufferRequestKey key;
	DsgBufferControlResponse response;
	StringInfoData output;
	DsgBufferAccess access;
	bool		guarded;
	int			token;

	DsgTestControlCheck();
	action = text_to_cstring(PG_GETARG_TEXT_PP(0));
	guarded = strncmp(action, "guarded_", 8) == 0;
	if (guarded)
		action += 8;
	MemSet(&request, 0, sizeof(request));
	request.boot_id = DsgTestControlUint64(PG_GETARG_TEXT_PP(1));
	request.issuer_epoch = DsgTestControlUint64(PG_GETARG_TEXT_PP(2));
	request.revision = DsgTestControlUint64(PG_GETARG_TEXT_PP(3));
	request.operation_id_high = DsgTestControlUint64(PG_GETARG_TEXT_PP(4));
	request.operation_id_low = DsgTestControlUint64(PG_GETARG_TEXT_PP(5));
	request.expected_generation = DsgTestControlUint64(PG_GETARG_TEXT_PP(6));
	request.target_buffers = PG_GETARG_INT32(7);
	token = PG_GETARG_INT32(10);
	if (token < 0 || token > 255)
		ereport(ERROR, (errmsg("invalid DSGA test context token")));
	MemSet(request.context_hash, token, sizeof(request.context_hash));
	key.boot_id = request.boot_id;
	key.issuer_epoch = request.issuer_epoch;
	key.revision = request.revision;
	key.operation_id_high = request.operation_id_high;
	key.operation_id_low = request.operation_id_low;
	MemSet(&response, 0, sizeof(response));

	/*
	 * 仅为错误锁序负例取得真实Guard，原生ERROR须释放它而非测试伪造状态。
	 */
	if (guarded)
		access = DsgBufferAccessBegin();
	if (strcmp(action, "resize") == 0)
		DsgBufferControlResize(&request, PG_GETARG_INT32(8), PG_GETARG_INT32(9), &response);
	else if (strcmp(action, "status") == 0)
		DsgBufferControlStatus(&key, &response);
	else if (strcmp(action, "cancel") == 0)
		DsgBufferControlCancel(&key, &response);
	else if (strcmp(action, "state") == 0)
		DsgBufferGetSnapshot(&response.observed);
	else
		ereport(ERROR, (errmsg("invalid DSGA control test action")));
	if (guarded)
		DsgBufferAccessEnd(&access);
	initStringInfo(&output);
	appendStringInfo(&output,
					 "{\"disposition\":%d,\"has_result\":%s,\"key\":{"
					 "\"boot\":\"" UINT64_FORMAT "\",\"epoch\":\"" UINT64_FORMAT
					 "\",\"revision\":\"" UINT64_FORMAT "\",\"hi\":\"" UINT64_FORMAT
					 "\",\"lo\":\"" UINT64_FORMAT "\"},\"result\":",
					 response.receipt.disposition, response.receipt.has_result ? "true" : "false",
					 response.receipt.key.boot_id, response.receipt.key.issuer_epoch,
					 response.receipt.key.revision, response.receipt.key.operation_id_high,
					 response.receipt.key.operation_id_low);
	if (response.receipt.has_result)
		DsgTestControlSnapshot(&output, &response.receipt.result);
	else
		appendStringInfoString(&output, "null");
	appendStringInfoString(&output, ",\"observed\":");
	DsgTestControlSnapshot(&output, &response.observed);
	appendStringInfoChar(&output, '}');
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(output.data)));
}

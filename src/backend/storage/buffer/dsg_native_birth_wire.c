/*-
 * dsg_native_birth_wire.c -- NDSGBR01 v1原生早期出生传输（原生接线组件）。
 *
 * main顺序必须为AS -> R -> Prepare -> PMbase -> Exchange/close198 -> PMfinal
 * -> Finish/Root最终ptrace复核停点 -> PG配置/插件；Root在对端须先完成原Pending、真实AS/maps/完整filter、
 * 同PID迁组和Enrolled fsync才可ACK。这里不产生normal/FS/持续包络资格。
 * 测试模型uid0回包仅能验证codec/transport，不能当真实生产Root封存。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "postgres.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "storage/dsg_envelope_birth.h"
#include "storage/dsg_envelope_filter.h"
#include "storage/dsg_native_birth_wire.h"

static DsgNativeBirthWireSnapshot state;
static DsgBirthArraysSnapshot original_arrays;
static bool prepared;
static bool finished;
static bool connected;
static pid_t root_peer_tgid;

/* 早期尚无MemoryContext，错误不分配、不elog、不降为test或legacy。 */
static pg_attribute_noreturn() void
WireFail(const char *reason)
{
	const char *prefix = "DSGA native birth wire: ";
	if (connected)
		(void) close(DSG_BIRTH_WIRE_FD);
	(void) write(STDERR_FILENO, prefix, strlen(prefix));
	(void) write(STDERR_FILENO, reason, strlen(reason));
	(void) write(STDERR_FILENO, "\n", 1);
	_exit(1);
}

static uint64
Decimal(const char *raw, uint64 maximum)
{
	uint64 value = 0;
	const char *p;
	if (raw == NULL || raw[0] < '1' || raw[0] > '9')
		WireFail("canonical positive original decimal required");
	for (p = raw; *p; p++)
	{
		unsigned digit;
		if (*p < '0' || *p > '9')
			WireFail("canonical positive original decimal required");
		digit = *p - '0';
		if (value > (maximum - digit) / 10)
			WireFail("original decimal overflow");
		value = value * 10 + digit;
	}
	return value;
}

static void
Digest(const char *raw, uint8 out[32])
{
	int i;
	bool nonzero = false;
	if (raw == NULL || strnlen(raw, 65) != 64)
		WireFail("original nonce and two digests must be lowercase hex32");
	for (i = 0; i < 32; i++)
	{
		unsigned octet = 0;
		int j;
		for (j = 0; j < 2; j++)
		{
			unsigned digit;
			char c = raw[2 * i + j];
			if (c >= '0' && c <= '9')
				digit = c - '0';
			else if (c >= 'a' && c <= 'f')
				digit = c - 'a' + 10;
			else
				WireFail("noncanonical original digest");
			octet = (octet << 4) | digit;
		}
		out[i] = octet;
		nonzero |= octet != 0;
	}
	if (!nonzero)
		WireFail("zero original nonce or digest is not a source");
}

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/un.h>

#define REQUEST_BYTES 232
#define ACK_BYTES 112

StaticAssertDecl(DSG_BUFFER_ARRAY_COUNT == 4, "native birth wire ABI1 requires four arrays");

/* 字节序显式编码，不依赖C结构体padding、对齐或编译器宿主端序。 */
static void
Put32(uint8 *at, uint32 value)
{
	int i;
	for (i = 3; i >= 0; i--)
	{
		at[i] = value & 255;
		value >>= 8;
	}
}

static void
Put64(uint8 *at, uint64 value)
{
	int i;
	for (i = 7; i >= 0; i--)
	{
		at[i] = value & 255;
		value >>= 8;
	}
}

static void
Header(uint8 *raw, uint32 kind)
{
	memcpy(raw, "NDSGBR01", 8);
	Put32(raw + 8, 1);
	Put32(raw + 12, kind);
	memcpy(raw + 16, state.nonce, 32);
	memcpy(raw + 48, state.source_sha256, 32);
	memcpy(raw + 80, state.profile_sha256, 32);
}

static void
CheckArrays(const DsgBirthArraysSnapshot *a)
{
	uint64 total = 0;
	int i;
	if (!a->enabled || a->maximum_buffers <= 0 || a->page_size_bytes != 4096 ||
		a->address_space_max_bytes == 0 || a->address_space_max_bytes > PG_INT64_MAX ||
		a->address_space_max_bytes % (64 << 10) != 0)
		WireFail("actual early array profile geometry missing");
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		const DsgBirthArrayRange *r = &a->ranges[i];
		uint64 base = (uint64) (uintptr_t) r->region.address;
		uint64 length;
		int j;
		if (r->element_bytes == 0 || r->element_bytes > (1 << 20) ||
			(uint64) a->maximum_buffers > (PG_UINT64_MAX - 4095) / r->element_bytes)
			WireFail("actual early array multiplication invalid");
		length = ((uint64) a->maximum_buffers * r->element_bytes + 4095) & ~UINT64CONST(4095);
		if (length != r->region.mapped_bytes || base < 4096 || base % 4096 != 0 ||
			base >= (UINT64CONST(1) << 47) || length > (UINT64CONST(1) << 47) - base ||
			length > PG_UINT64_MAX - total)
			WireFail("actual early array address or reservation invalid");
		for (j = 0; j < i; j++)
		{
			uint64 previous = (uint64) (uintptr_t) a->ranges[j].region.address;
			if (base < previous + a->ranges[j].region.mapped_bytes && previous < base + length)
				WireFail("actual early reservations overlap");
		}
		total += length;
	}
	if (total != a->reserved_total_bytes || total >= a->address_space_max_bytes)
		WireFail("actual early total reservation differs");
}

static void
CheckPeer(void)
{
	struct ucred peer;
	socklen_t length = sizeof(peer);
	int type;
	socklen_t type_length = sizeof(type);
	if (getsockopt(DSG_BIRTH_WIRE_FD, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 ||
		length != sizeof(peer) || peer.uid != 0 || peer.pid <= 0 ||
		getsockopt(DSG_BIRTH_WIRE_FD, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0 ||
		type_length != sizeof(type) || type != SOCK_SEQPACKET)
		WireFail("fixed birth socket peer is not root seqpacket");
	if (root_peer_tgid != 0 && root_peer_tgid != peer.pid)
		WireFail("original Root peer TGID changed");
	root_peer_tgid = peer.pid;
}


/* /proc是本步骤唯一内核身份来源；使用有界栈缓冲，不依赖尚未建立的PG上下文。
 * 所有FD都在SIGSTOP前关闭，Root最终审计必须再次核198与原PID/start身份。
 * 原SO_PEERCRED.pid是TGID，而TracerPid可能是LockOSThread的Root线程TID，不能相等比较。
 */
static void
ReadStatus(const char *path, char status[8192])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	size_t used = 0;
	if (fd < 0)
		WireFail("cannot read actual tracer status");
	for (;;)
	{
		ssize_t got;
		if (used == 8191)
			WireFail("actual tracer status exceeds bounded parser");
		do { got = read(fd, status + used, 8191 - used); } while (got < 0 && errno == EINTR);
		if (got < 0)
			WireFail("cannot read actual tracer status");
		if (got == 0)
			break;
		used += got;
	}
	if (close(fd) != 0)
		WireFail("tracer status descriptor did not close");
	status[used] = '\0';
	if (used == 0 || status[used - 1] != '\n' || memchr(status, '\0', used) != NULL)
		WireFail("actual tracer status malformed");
}

/* /proc数字以十进制输出；只接受准确字段、准确项数和行尾，不用sscanf宽松匹配。 */
static void
StatusValues(const char *status, const char *field, uint64 values[4], int count)
{
	const char *line = status;
	size_t length = strlen(field);
	bool found = false;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		if (end == NULL)
			WireFail("actual tracer status lacks line ending");
		if ((size_t) (end - line) > length && strncmp(line, field, length) == 0)
		{
			const char *cursor = line + length;
			int i;
			if (found)
				WireFail("duplicate actual tracer status field");
			found = true;
			for (i = 0; i < count; i++)
			{
				uint64 value = 0;
				bool digit = false;
				while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
					cursor++;
				while (cursor < end && *cursor >= '0' && *cursor <= '9')
				{
					unsigned next = *cursor++ - '0';
					if (value > (INT_MAX - next) / 10)
						WireFail("actual tracer status numeric overflow");
					value = value * 10 + next;
					digit = true;
				}
				if (!digit || (cursor < end && *cursor != ' ' && *cursor != '\t'))
					WireFail("actual tracer status numeric format differs");
				values[i] = value;
			}
			while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
				cursor++;
			if (cursor != end)
				WireFail("actual tracer status has extra values");
		}
		line = end + 1;
	}
	if (!found)
		WireFail("actual tracer status field missing");
}

static pid_t
ActualTracer(void)
{
	char status[8192];
	uint64 values[4];
	ReadStatus("/proc/self/status", status);
	StatusValues(status, "TracerPid:", values, 1);
	return (pid_t) values[0];
}

static void
FinalAuditGate(void)
{
	char path[64];
	char status[8192];
	uint64 values[4];
	pid_t tracer = ActualTracer();
	int i;
	if (tracer <= 0 || root_peer_tgid <= 0 || syscall(SYS_gettid) != (long) getpid())
		WireFail("PMfinal requires original Root tracer before PG initialization");
	if (snprintf(path, sizeof(path), "/proc/%ld/status", (long) tracer) <= 0)
		WireFail("cannot address actual Root tracer");
	ReadStatus(path, status);
	StatusValues(status, "Pid:", values, 1);
	if (values[0] != (uint64) tracer)
		WireFail("actual Root tracer TID differs");
	StatusValues(status, "Tgid:", values, 1);
	if (values[0] != (uint64) root_peer_tgid)
		WireFail("actual tracer TGID differs from original Root peer");
	StatusValues(status, "Uid:", values, 4);
	for (i = 0; i < 4; i++)
		if (values[i] != 0)
			WireFail("actual Root tracer credentials differ");
	/* 不持PG锁/目录锁。Root已在ACK前SEIZE+EXITKILL；dumpable0不可撤回。
	 * tgkill触发signal-delivery-stop，Root完成实际BPF/AS/R/FD复核和WAL fsync后
	 * 必须PTRACE_DETACH(signal=0)抑制SIGSTOP并释放。单独CONT或SIGCONT不是释放。
	 * 本段不产生Root持久资格、FS/I包络或normal许可；错误只退出，绝不回退。
	 */
	if (prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0 ||
		prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L) != 0 || ActualTracer() != tracer)
		WireFail("original Root tracer or nondumpable state changed before stop");
	state.phase = "pmfinal_waiting_root_detach_non_authorizing";
	if (syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGSTOP) != 0)
		WireFail("cannot establish final Root audit stop");
	if (ActualTracer() != 0 || prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L) != 0)
		WireFail("only original Root detach may release final audit gate");
	state.final_audit_released_non_authorizing = true;
}

/* 所有收到的SCM_RIGHTS均关闭，包括MSG_CTRUNC可见前缀；超出控制缓冲区的
 * fd由Linux截断路径关闭。任何ancillary本身都拒绝，不将FD当出生事实。 */

/* Root的PMbase实际内核审计需要ptrace中断recvmsg。SO_RCVTIMEO下该中断可能
 * 返回EINTR，不能把正确Root审计误作坏帧。每次重试重设剩余绝对预算，信号
 * 不可把30秒延长；错误/超时仍关门，不降为test，也不放宽任何ACK/FD校验。
 */
static ssize_t
ReceiveACK(struct msghdr *message)
{
	struct timespec started;
	size_t control_bytes = message->msg_controllen;
	if (clock_gettime(CLOCK_MONOTONIC, &started) != 0)
		WireFail("cannot establish bounded ACK receive clock");
	for (;;)
	{
		struct timespec now;
		struct timeval remaining;
		int64 nanoseconds;
		ssize_t received;
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			WireFail("cannot read bounded ACK receive clock");
		nanoseconds = ((int64) started.tv_sec + 30 - (int64) now.tv_sec) *
			INT64CONST(1000000000) + started.tv_nsec - now.tv_nsec;
		if (nanoseconds <= 0)
			WireFail("original Root ACK absolute deadline expired");
		remaining.tv_sec = nanoseconds / INT64CONST(1000000000);
		remaining.tv_usec = (nanoseconds % INT64CONST(1000000000) + 999) / 1000;
		if (remaining.tv_usec == 1000000)
		{
			remaining.tv_sec++;
			remaining.tv_usec = 0;
		}
		if (setsockopt(DSG_BIRTH_WIRE_FD, SOL_SOCKET, SO_RCVTIMEO,
					   &remaining, sizeof(remaining)) != 0)
			WireFail("cannot set remaining original ACK deadline");
		message->msg_controllen = control_bytes;
		message->msg_flags = 0;
		received = recvmsg(DSG_BIRTH_WIRE_FD, message, MSG_CMSG_CLOEXEC);
		if (received < 0 && errno == EINTR)
			continue;
		return received;
	}
}

static void
CloseRights(struct msghdr *message)
{
	struct cmsghdr *control;
	for (control = CMSG_FIRSTHDR(message); control != NULL; control = CMSG_NXTHDR(message, control))
	{
		if (control->cmsg_len < CMSG_LEN(0))
			break;
		if (control->cmsg_level == SOL_SOCKET && control->cmsg_type == SCM_RIGHTS)
		{
			size_t bytes = control->cmsg_len - CMSG_LEN(0);
			size_t offset;
			for (offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int))
			{
				int fd;
				memcpy(&fd, (char *) CMSG_DATA(control) + offset, sizeof(fd));
				(void) close(fd);
			}
		}
	}
}
#endif

void
DsgNativeBirthWirePrepare(void)
{
	const char *profile = getenv(DSG_BIRTH_ARRAYS_PROFILE_ENV);
	const char *filter = getenv(DSG_BIRTH_FILTER_ENV);
	if (prepared)
		WireFail("prepare may run once only");
	prepared = true;
	state.protocol_version = DSG_NATIVE_BIRTH_WIRE_VERSION;
	state.phase = "disabled";
	state.original_postmaster_pid = getpid();
	if (profile == NULL || strncmp(profile, "root-v1:", 8) != 0)
	{
		/* 上游A/R亦严格校验mode；本独立模块不能把未知root拼写当disabled。 */
		if (profile != NULL && strncmp(profile, "test-v1:", 8) != 0)
			WireFail("unknown native birth array mode");
		if (getenv(DSG_NATIVE_BIRTH_NONCE_ENV) != NULL || getenv(DSG_NATIVE_BIRTH_SOURCE_ENV) != NULL ||
			getenv(DSG_NATIVE_BIRTH_PROFILE_ENV) != NULL || getenv(DSG_NATIVE_BIRTH_STARTUP_ENV) != NULL ||
			(filter != NULL && strcmp(filter, "root-v1") == 0))
			WireFail("root source fields require exact root-v1 array and filter mode");
		return;
	}
	if (filter == NULL || strcmp(filter, "root-v1") != 0)
		WireFail("root-v1 requires original root-v1 filter");
	Digest(getenv(DSG_NATIVE_BIRTH_NONCE_ENV), state.nonce);
	Digest(getenv(DSG_NATIVE_BIRTH_SOURCE_ENV), state.source_sha256);
	Digest(getenv(DSG_NATIVE_BIRTH_PROFILE_ENV), state.profile_sha256);
	state.startup_buffers = (int) Decimal(getenv(DSG_NATIVE_BIRTH_STARTUP_ENV), INT_MAX);
	DsgBirthArraysGetSnapshot(&original_arrays);
	if ((int) Decimal(profile + 8, INT_MAX) != original_arrays.maximum_buffers ||
		state.startup_buffers > original_arrays.maximum_buffers)
		WireFail("original M/N0 differs from actual reservation");
#if defined(__linux__) && defined(__x86_64__)
	{
		struct sockaddr_un address;
		struct timeval timeout = {30, 0};
		int fd;
		CheckArrays(&original_arrays);
		/* 不能静默覆盖任意继承FD。launcher须close caller FD并允许NOFILE>198。 */
		errno = 0;
		if (fcntl(DSG_BIRTH_WIRE_FD, F_GETFD) != -1 || errno != EBADF)
			WireFail("reserved birth descriptor was already open");
		fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (fd < 0)
			WireFail("cannot create original birth socket");
		if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
			WireFail("cannot set bounded birth socket timeouts");
		memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		memcpy(address.sun_path, DSG_NATIVE_BIRTH_SOCKET, sizeof(DSG_NATIVE_BIRTH_SOCKET));
		if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0)
			WireFail("cannot connect original root birth socket");
		if (fd != DSG_BIRTH_WIRE_FD)
		{
			if (dup3(fd, DSG_BIRTH_WIRE_FD, O_CLOEXEC) < 0)
				WireFail("cannot reserve fixed birth descriptor");
			(void) close(fd);
		}
		connected = true;
		CheckPeer();
		if (fcntl(DSG_BIRTH_WIRE_FD, F_GETFD) != FD_CLOEXEC)
			WireFail("fixed birth descriptor is not CLOEXEC");
		state.enabled = true;
		state.phase = "connected_not_enrolled";
	}
#else
	WireFail("root birth wire requires supported Linux amd64");
#endif
}

void
DsgNativeBirthWireExchange(void)
{
	if (!prepared || finished)
		WireFail("exchange before prepare or after finish");
	if (!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	{
		uint8 request[REQUEST_BYTES] = {0};
		uint8 ack[ACK_BYTES + 1] = {0};
		uint8 expected[ACK_BYTES] = {0};
		union { struct cmsghdr alignment; char bytes[CMSG_SPACE(16 * sizeof(int))]; } control;
		struct msghdr message;
		struct iovec vector;
		DsgBirthFilterSnapshot filter;
		ssize_t received;
		int i;
		DsgBirthFilterGetSnapshot(&filter);
		if (getpid() != state.original_postmaster_pid || !connected || state.acknowledged ||
			!filter.enabled || !filter.postmaster_installed ||
			filter.child_installed || filter.finalized)
			WireFail("exchange lacks original open PMbase filter");
		Header(request, 1);
		Put64(request + 112, original_arrays.address_space_max_bytes);
		Put32(request + 120, original_arrays.maximum_buffers);
		Put32(request + 124, original_arrays.page_size_bytes);
		Put64(request + 128, original_arrays.reserved_total_bytes);
		for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
		{
			Put64(request + 136 + 24 * i, (uint64) (uintptr_t) original_arrays.ranges[i].region.address);
			Put64(request + 144 + 24 * i, original_arrays.ranges[i].region.mapped_bytes);
			Put64(request + 152 + 24 * i, original_arrays.ranges[i].element_bytes);
		}
		CheckPeer();
		if (send(DSG_BIRTH_WIRE_FD, request, sizeof(request), MSG_NOSIGNAL) != (ssize_t) sizeof(request))
			WireFail("exact native birth request was not sent");
		memset(&message, 0, sizeof(message));
		memset(&control, 0, sizeof(control));
		vector.iov_base = ack;
		vector.iov_len = sizeof(ack);
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control.bytes);
		received = ReceiveACK(&message);
		if (received >= 0)
			CloseRights(&message);
		Header(expected, 2);
		if (received != ACK_BYTES || message.msg_controllen != 0 ||
			(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || memcmp(ack, expected, ACK_BYTES) != 0)
			WireFail("root birth ACK missing, changed, oversized or ancillary");
		CheckPeer();
		/* 先真实close固定FD，再让main安装PMfinal；ACK此刻仍非授权事实。 */
		if (close(DSG_BIRTH_WIRE_FD) != 0)
			WireFail("fixed birth descriptor did not close");
		connected = false;
		state.acknowledged = true;
		state.phase = "acknowledged_not_finalized_non_authorizing";
	}
#else
	WireFail("root birth exchange unsupported");
#endif
}

void
DsgNativeBirthWireFinish(void)
{
	if (!prepared || finished)
		WireFail("finish must follow original one-time prepare");
	finished = true;
	if (!state.enabled)
		return;
#if defined(__linux__) && defined(__x86_64__)
	{
		DsgBirthFilterSnapshot filter;
		DsgBirthFilterGetSnapshot(&filter);
		errno = 0;
		if (getpid() != state.original_postmaster_pid || !state.acknowledged || connected ||
			!filter.enabled || !filter.postmaster_installed ||
			!filter.finalized || filter.child_installed ||
			fcntl(DSG_BIRTH_WIRE_FD, F_GETFD) != -1 || errno != EBADF)
			WireFail("original birth descriptor or PMfinal state differs");
		FinalAuditGate();
		state.source_bound = true;
		state.root_enrolled_non_authorizing = true;
		state.phase = "root_enrolled_non_authorizing";
	}
#else
	WireFail("root birth finish unsupported");
#endif
}

void
DsgNativeBirthWireGetSnapshot(DsgNativeBirthWireSnapshot *snapshot)
{
	/* fork继承的是原PM出生事实，original_postmaster_pid不改成当前child PID。
	 * 任何消费者必须保留该来源身份，不能把child当另一次独立Root握手。 */
	*snapshot = state;
}

bool
DsgNativeBirthWireRootPrepared(void)
{
	return prepared && state.enabled && connected && !state.acknowledged && !finished &&
		getpid() == state.original_postmaster_pid;
}

bool
DsgNativeBirthWireRootACKClosed(void)
{
	if (!prepared || !state.enabled || !state.acknowledged || connected || finished ||
		getpid() != state.original_postmaster_pid)
		return false;
#if defined(__linux__) && defined(__x86_64__)
	errno = 0;
	return fcntl(DSG_BIRTH_WIRE_FD, F_GETFD) == -1 && errno == EBADF;
#else
	return false;
#endif
}

bool
DsgNativeBirthWireCheckConfiguration(int maximum, int startup)
{
	return prepared && (!state.enabled || (finished && state.root_enrolled_non_authorizing &&
		state.final_audit_released_non_authorizing &&
		getpid() == state.original_postmaster_pid &&
		maximum == original_arrays.maximum_buffers && startup == state.startup_buffers));
}

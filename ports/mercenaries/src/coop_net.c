#include "coop_net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")

static SOCKET				g_sock = INVALID_SOCKET;
static struct sockaddr_in	g_peer;
static HANDLE				g_thread;
static volatile LONG		g_stop;
static volatile LONG		g_active;
static SRWLOCK				g_lock = SRWLOCK_INIT;
static CoopSnapshot			g_latest;
static int					g_have_latest;
static ULONGLONG			g_latest_ms;

static void set_error(char* error, unsigned size, const char* text)
{
	if (error && size) snprintf(error, size, "%s", text);
}

static int finite_all(const float* v, unsigned count, float limit)
{
	for (unsigned i = 0; i < count; ++i)
		if (!isfinite(v[i]) || fabsf(v[i]) > limit) return 0;
	return 1;
}

static int packet_valid(const CoopSnapshot* p)
{
	return p->magic == COOP_MAGIC && p -> version == COOP_VERSION &&
		p->size == sizeof(CoopSnapshot) &&
		finite_all(p->position, 3, 1.0e6f) &&
		finite_all(p->rotation, 12, 1.0e3f);
}

static DWORD WINAPI recv_thread(LPVOID unused)
{
	(void)unused;
	char buffer[256];
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		struct sockaddr_in from;
		int from_len = (int)sizeof(from);
		int n = recvfrom(g_sock, buffer, (int)sizeof(buffer), 0, (struct sockaddr*)&from, &from_len);
		if (n < 0) {
			if (WSAGetLastError() != WSAETIMEDOUT) Sleep(10);
			continue;
		}
		if (n != (int)sizeof(CoopSnapshot)) continue;
		if (from.sin_addr.s_addr != g_peer.sin_addr.s_addr ||
			from.sin_port != g_peer.sin_port) continue;
		CoopSnapshot packet;
		memcpy(&packet, buffer, sizeof(packet));
		if (!packet_valid(&packet)) continue;

		ULONGLONG now = GetTickCount64();
		AcquireSRWLockExclusive(&g_lock);

		if (!g_have_latest || (int32_t)(packet.sequence - g_latest.sequence) > 0 ||
			now - g_latest_ms > 1500)
		{
			g_latest = packet;
			g_have_latest = 1;
			g_latest_ms = now;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
	return 0;
}

int coop_net_start(const char* spec, char* error, unsigned error_size)
{
	//Check for active network already running
	if (InterlockedCompareExchange(&g_active, 0, 0))
	{
		set_error(error, error_size, "co-op network already started");
		return 0;
	}

	//Parse spec string ("<local port>,<host>:<peer port> 
	unsigned short local_port = 0, peer_port = 0;
	char host[64] = { 0 };
	if (!spec || sscanf(spec, " %hu , %63[^:]:%hu", &local_port, host, &peer_port) != 3
		|| !local_port || !peer_port) 
	{ 
		set_error(error, error_size, "expected LOCAL_PORT,PEER_IPV4:PEER_PORT");
		return 0; 
	}

	//Build peer address
	memset(&g_peer, 0, sizeof(g_peer));
	g_peer.sin_family = AF_INET;
	g_peer.sin_port = htons(peer_port);
	if (inet_pton(AF_INET, host, &g_peer.sin_addr) != 1)
	{
		set_error(error, error_size, "peer address must be a dotted IPv4 address");
		return 0;
	}

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		set_error(error, error_size, "WSAStartup failed");
		return 0;
	}
	g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_sock == INVALID_SOCKET)
	{
		set_error(error, error_size, "socket() failed");
		WSACleanup();
		return 0;
	}
	DWORD timeout_ms = 100;
	setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
	
	//Loopback test
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(local_port);
	local.sin_addr.s_addr = (g_peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
	if (bind(g_sock, (struct sockaddr*)&local, sizeof(local)) != 0)
	{
		set_error(error, error_size, "bind() failed; local port may be in use.");
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		WSACleanup();
		return 0;
	}

	g_stop = 0;
	g_have_latest = 0;
	g_thread = CreateThread(NULL, 0, recv_thread, NULL, 0, NULL);
	if (!g_thread)
	{
		set_error(error, error_size, "CreateThread failed");
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
		WSACleanup();
		return 0;
	}
	InterlockedExchange(&g_active, 1);
	return 1;
}

void coop_net_stop(void)
{
	if (!InterlockedCompareExchange(&g_active, 0, 0)) return;
	InterlockedExchange(&g_stop, 1);
	if (g_thread)
	{
		WaitForSingleObject(g_thread, 2000);
		CloseHandle(g_thread);
		g_thread = NULL;
	}
	if (g_sock != INVALID_SOCKET) closesocket(g_sock);
	g_sock = INVALID_SOCKET;
	WSACleanup();
	InterlockedExchange(&g_active, 0);
}

int coop_net_active(void)
{
	return InterlockedCompareExchange(&g_active, 0, 0) != 0;
}

void coop_net_send(const CoopSnapshot* snapshot)
{
	if (!coop_net_active() || g_sock == INVALID_SOCKET) return;
	sendto(g_sock, (const char*)snapshot, (int)sizeof(*snapshot), 0,
		(const struct sockaddr*)&g_peer, (int)sizeof(g_peer));
}

int coop_net_take_latest(CoopSnapshot* out, uint32_t* last_sequence)
{
	int taken = 0;
	AcquireSRWLockShared(&g_lock);
	if (g_have_latest && g_latest.sequence != *last_sequence)
	{
		*out = g_latest;
		*last_sequence = g_latest.sequence;
		taken = 1;
	}
	ReleaseSRWLockShared(&g_lock);
	return taken;
}


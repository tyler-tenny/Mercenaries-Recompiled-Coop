// Co-op testing: adopts most recent dev-spawned unit
// and attaches it to local player.
// Included by recomp_manual.c after dev_spawn.h
// Uses static helpers (dev_call, dev_hash, dev_player_human,
// dev_last_spawn_guid, dev_spawn_serial)

#include "coop_net.h"

static int		 coop_initialized, coop_enabled, coop_have_target;
static uint32_t  coop_rx_sequence;
static float	 coop_target_pos[3], coop_target_rot[12];
static uint32_t	 coop_tx_sequence;
static ULONGLONG coop_next_send_ms;

#ifndef MERCENARIES_COOP_PUPPET_H
#define MERCENARIES_COOP_PUPPET_H

//Game readiness check
//Guest memory mapped, stack pointer sane, world-ready markers set.
//Same readiness check as dev spawner

static int coop_in_gameplay(void)
{
	return g_xbox_mem_offset && g_esp >= 0x20000u && g_esp < 0x4000000u && 
		guest_u32(0x413F6C) == 0x4249D707u && guest_u32(0x413F68) == 0xC2CBD863u;
}

//Find local human/actor
//Controller = player0's controller object (retail fn 0x8C7E0).
//Human = the character actor it controls (controller+0x998).
//Returns 0 if not ready. Returns guest address.

static uint32_t coop_local_human(uint32_t stack)
{
	uint32_t name = 0x660E4490u;
	uint32_t controller = dev_call(stack, 0x0008C7E0u, 0, 1, &name);
	return dev_player_human(controller);
}

//Read local position
//Calls GetPosition (vtable slot +0x34) via dev_call.
//Place result in scratch buffer on stack, then copy to host memory.

static int coop_local_position(uint32_t stack, uint32_t human, float* out)
{
	if (!dev_guest_address(human, 0x76C)) return 0;
	uint32_t vtable = guest_u32(human);
	if (!dev_guest_address(vtable, 0x40) || !recomp_lookup(guest_u32(vtable + 0x34))) return 0;
	uint32_t buffer = stack + 0x100;
	dev_call(stack, guest_u32(vtable + 0x34), human, 1, &buffer);
	memcpy(out, guest_ptr(buffer), 12);
	return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static uint32_t puppet_guid, puppet_name, adopted_guid;

//Keep checking actor address to prevent invalid access.
//Look up by GUID each tick and verify name hash (actor+4) to confirm.

static uint32_t coop_puppet_actor(uint32_t stack)
{
	if (!puppet_guid) return 0;
	uint32_t guid = puppet_guid;
	uint32_t actor = dev_call(stack, 0x001EC220u, 0, 1, &guid);
	return dev_guest_address(actor, 0x300) && guest_u32(actor + 4) == puppet_name ? actor : 0;
}

//Per tick loop for coop functionality
//Called from recomp_dev_spawn_tick() at world-update checkpoint

void recomp_coop_tick(void)
{
	//guard for game readiness and re-entry
	static int executing;
	if (executing || !coop_in_gameplay()) return;

	//start network for coop
	if (!coop_initialized) 
	{
		coop_initialized = 1;
		char spec[128];
		DWORD n = GetEnvironmentVariableA("MERC_COOP", spec, sizeof(spec));
		if (n > 0 && n < sizeof(spec))
		{
			char error[128] = { 0 };
			coop_enabled = coop_net_start(spec, error, sizeof(error));
			xbox_preview_log_event("coop", "net start ok=%d %s", coop_enabled, error);
		}
	}

	//Save CPU state and exits with previous registers restored
	//NO EARLY RETURNS between here and restore context
	recomp_saved_guest_cpu_context saved;
	recomp_save_guest_cpu_context(&saved);
	executing = 1;
	uint32_t stack = (g_esp - 0x4000u) & ~15u;

	//Adopt newest dev menu spawn as puppet
	if (dev_last_spawn_guid && dev_last_spawn_guid != adopted_guid)
	{
		char name[48];
		snprintf(name, sizeof(name), "recomp_spawn_%u", dev_spawn_serial);
		puppet_guid = adopted_guid = dev_last_spawn_guid;
		puppet_name = dev_hash(name);
	}

	//Read local player position
	uint32_t human = coop_local_human(stack);
	float pos[3];
	int have_pos = coop_local_position(stack, human, pos);
	if (have_pos)
		xbox_preview_log_event("coop", "local %.2f %.2f %.2f", pos[0], pos[1], pos[2]);

	if (coop_enabled && human && have_pos && GetTickCount64() >= coop_next_send_ms)
	{
		CoopSnapshot out;
		memset(&out, 0, sizeof(out));
		out.magic = COOP_MAGIC;
		out.version = COOP_VERSION;
		out.size = sizeof(out);
		out.sequence = ++coop_tx_sequence;
		out.map_hash = guest_u32(0x403970u);
		memcpy(out.position, pos, 12);
		memcpy(out.rotation, guest_ptr(human + 0xB0), 48);
		coop_net_send(&out);
		coop_next_send_ms = GetTickCount64() + 50;
	}
	//Use newest packet
	if (coop_enabled)
	{
		CoopSnapshot in;
		if (coop_net_take_latest(&in, &coop_rx_sequence) && in.map_hash == guest_u32(0x403970u))
		{
			memcpy(coop_target_pos, in.position, 12);
			memcpy(coop_target_rot, in.rotation, 48);
			coop_have_target = 1;
		}
	}

	//Look up puppet actor
	uint32_t actor = coop_puppet_actor(stack);

	if (actor)
	{
		float target[3];
		int ok = 0;
		if (coop_have_target) {
			memcpy(target, coop_target_pos, 12);
			memcpy(guest_ptr(actor + 0xB0), coop_target_rot, 48);
			ok = 1;
		}
		else if (human && have_pos) {
			target[0] = pos[0] + 3.0f; target[1] = pos[1]; target[2] = pos[2];
			memcpy(guest_ptr(actor + 0xB0), guest_ptr(human + 0xB0), 48);
			ok = 1;
		}
		if (ok) {
			uint32_t position = stack + 0x100;
			memcpy(guest_ptr(position), target, 12);
			uint32_t set_position = guest_u32(guest_u32(actor) + 0x64);
			if (recomp_lookup(set_position)) dev_call(stack, set_position, actor, 1, &position);
		}
	}

	xbox_preview_log_event("coop", "map %08X", guest_u32(0x403970u));

	//Restore CPU context
	executing = 0;
	recomp_restore_guest_cpu_context(&saved);
}
#endif
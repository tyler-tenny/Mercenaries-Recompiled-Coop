#ifndef MERCENARIES_COOP_NET_H
#define MERCENARIES_COOP_NET_H
#include <stdint.h>

#define COOP_MAGIC   0x504F434Du
#define COOP_VERSION 1u

#pragma pack(push, 1)
typedef struct CoopSnapshot
{
	uint32_t magic;			// 0x504F434D
	uint16_t version;		// 1
	uint16_t size;			// sizeof(CoopSnapshot)
	uint32_t sequence;		// increment per packet
	uint32_t map_hash;		// guest_u32(0x403970)
	float	 position[3];	//world position of sender's actor
	float	 rotation[12];	//rotation of sender's actor
} CoopSnapshot;
#pragma pack(pop)

int coop_net_start(const char* spec, char* error, unsigned error_size);
void coop_net_stop(void);
int coop_net_active(void);
void coop_net_send(const CoopSnapshot* snapshot);
int coop_net_take_latest(CoopSnapshot* out, uint32_t* last_sequence);
#endif
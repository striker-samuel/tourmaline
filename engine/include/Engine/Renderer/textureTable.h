#pragma once 

#include <Engine/Renderer/context.h>

//for the global descriptor list you must track on host / cpu side, with texture_asset_id(the #id of the texture in the project) to the table id, track when the draw doesnt need it anymore and use the cpu table to partial udpate evict texture in the bindless table right
struct TextureTable {
    static constexpr uint32_t kListEndSentinel = 0xffffffff;
	uint32_t freeHead;
	uint32_t map_[MAX_TEXTURES];
};

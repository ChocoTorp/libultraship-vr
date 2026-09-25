#pragma once

// QuestShip: on-device texture-pack optimizer. Converts the HD RGBA textures of installed .otr
// texture packs (e.g. Djipi's 3DS Experience) to GPU-compressed ASTC with prebuilt mip chains, in
// the background while the game runs, and writes them to mods/zz_questship_astc.o2r. That archive
// loads after the packs (last archive wins), so from the next launch the renderer uploads ASTC
// (4x less GPU memory) while the original pack stays installed as the source of CPU-side pixels.
// Same output format as tools/texconv (OTEX resource version 2).

#include <string>
#include <vector>

namespace Ship {
namespace TexturePackOptimizer {

// Name (without extension) of the generated archive in the mods folder.
extern const char* const kOutputName;

// Call BEFORE the mods folder is scanned. Removes an optimized archive that no longer matches the
// installed packs (a pack added, removed or replaced) and any unfinished output, so a stale
// conversion can never override the current packs.
void PrepareAtBoot(const std::string& modsDir);

// Call AFTER the mods are loaded, with their paths in load order. Starts the background conversion
// when packs are installed and no up-to-date optimized archive exists. Returns immediately.
void StartIfNeeded(const std::string& modsDir, const std::vector<std::string>& modPathsInLoadOrder);

// One-line human-readable status for the settings menu ("" when there is nothing to report).
std::string Status();

} // namespace TexturePackOptimizer
} // namespace Ship

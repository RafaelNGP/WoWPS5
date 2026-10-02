#pragma once
#include <cmath>
#include <cstdint>
namespace wowee::game {
// Console targeting is repeatable nearest selection, including quest NPCs.
// Compare canonical 3D positions; GUID resolves equal-distance ties.
template<class Player, class Npcs>
/// The nearest living creature in range - a hostile one when there is any, as
/// TARGETNEARESTENEMY means, and otherwise the nearest of the rest so the same
/// press still reaches a quest giver to talk to. Equal distances break by guid
/// so the choice is stable; a smaller guid never beats a nearer creature.
uint64_t nearestLivingLocalTarget(const Player& player, const Npcs& npcs, float range = 40.0f,
                                  uint64_t exclude = 0) {
    uint64_t best[2] = {0, 0};                 // [0] hostile, [1] anything
    float bestDistance[2] = {range * range, range * range};
    for (const auto& npc : npcs) {
        if (!npc.guid || npc.guid == exclude || npc.dead || npc.mapId != player.mapId || npc.instanceId != player.instanceId) continue;
        const float x = npc.x-player.x, y = npc.y-player.y, z = npc.z-player.z;
        const float d = x*x+y*y+z*z;
        if (!std::isfinite(d)) continue;
        for (int k = npc.hostile ? 0 : 1; k < 2; ++k) {
            if (d > bestDistance[k]) continue;
            if (!best[k] || d < bestDistance[k] || (d == bestDistance[k] && npc.guid < best[k])) {
                best[k] = npc.guid; bestDistance[k] = d;
            }
        }
    }
    return best[0] ? best[0] : best[1];
}
} // namespace wowee::game

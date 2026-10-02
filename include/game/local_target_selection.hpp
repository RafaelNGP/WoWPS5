#pragma once
#include <algorithm>
#include <vector>
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

/// R1's cycle, which has to reach everything a pad player needs to act on:
/// the enemy to fight, the NPC to talk to and the body to loot. The first press
/// takes a lootable body at arm's length, else the nearest enemy, else the
/// nearest creature; each press after moves to the next nearest after the
/// current one, round the whole list.
template <class Player, class Npcs>
uint64_t cycleLocalTarget(const Player& player, const Npcs& npcs, uint64_t current, float range = 40.0f) {
    struct Candidate { uint64_t guid; float d; bool hostile, loot; };
    std::vector<Candidate> list;
    for (const auto& npc : npcs) {
        if (!npc.guid || npc.mapId != player.mapId || npc.instanceId != player.instanceId) continue;
        if (npc.dead && !npc.lootable) continue;
        const float x = npc.x-player.x, y = npc.y-player.y, z = npc.z-player.z;
        const float d = x*x+y*y+z*z;
        if (!std::isfinite(d) || d > range*range) continue;
        list.push_back({npc.guid, d, npc.hostile && !npc.dead, npc.dead && npc.lootable});
    }
    if (list.empty()) return 0;
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        return a.d != b.d ? a.d < b.d : a.guid < b.guid;
    });
    const auto at = std::find_if(list.begin(), list.end(), [&](const Candidate& c) { return c.guid == current; });
    if (current && at != list.end()) return (at + 1 == list.end() ? list.front() : *(at + 1)).guid;
    for (const auto& c : list) if (c.loot && c.d <= 8.0f*8.0f) return c.guid;
    for (const auto& c : list) if (c.hostile) return c.guid;
    return list.front().guid;
}
} // namespace wowee::game

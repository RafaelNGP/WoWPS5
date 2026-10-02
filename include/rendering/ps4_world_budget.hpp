#pragma once
#include <algorithm>
#include <cmath>

namespace wowee::rendering::ps4budget {
// A fixed world working set for the console's flexible CPU heap. Camera
// rotation never changes residency; only travel crosses these tile rings.
#if defined(WOWEE_PS5)
// The PS5's title heap is direct memory (gigabytes, not the PS4's 448 MiB
// flexible pool): a 5x5 ring and twice the PS4 view distance by default.
inline constexpr float kDefaultViewDistance = 800.0f;
inline constexpr float kMinViewDistance = 400.0f;
inline constexpr float kMaxViewDistance = 1066.0f;
inline constexpr int kLoadRadius = 2;
inline constexpr int kUnloadRadius = 3;
inline constexpr unsigned kMaxResidentTiles = 30;
#else
inline constexpr float kDefaultViewDistance = 420.0f;
inline constexpr float kMinViewDistance = 400.0f;
inline constexpr float kMaxViewDistance = 533.0f;
inline constexpr int kLoadRadius = 1;
inline constexpr int kUnloadRadius = 2;
inline constexpr unsigned kMaxResidentTiles = 10;
#endif
// Square neighborhood includes diagonal collision/terrain at tile corners.
inline constexpr bool neededTile(int dx, int dy) {
    return dx >= -kLoadRadius && dx <= kLoadRadius && dy >= -kLoadRadius && dy <= kLoadRadius;
}
// Distance to the tile rectangle, in render coordinates (ADT X maps to Y).
// Residency depends on position, never camera direction: rotating in place
// must not destroy collision geometry or cause a reload storm.
inline float tileDistanceSquared(int tileX, int tileY, float cameraX, float cameraY) {
    constexpr float size = 1600.0f / 3.0f;
    const float maxX = (32 - tileY) * size, maxY = (32 - tileX) * size;
    const float dx = std::max({maxX - size - cameraX, 0.f, cameraX - maxX});
    const float dy = std::max({maxY - size - cameraY, 0.f, cameraY - maxY});
    return dx * dx + dy * dy;
}
inline bool tileInRange(int tileX, int tileY, float cameraX, float cameraY,
                        float viewDistance, bool resident) {
    const float radius = std::clamp(viewDistance, kMinViewDistance, kMaxViewDistance)
                       + (resident ? 96.f : 32.f);
    return tileDistanceSquared(tileX, tileY, cameraX, cameraY) <= radius * radius;
}
inline constexpr unsigned kMaxLocalLights = 8;
} // namespace wowee::rendering::ps4budget

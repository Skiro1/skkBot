#pragma once

#include <array>
#include <cstdint>
#include <cstring>

// Отдельный диагностический формат, не часть SKK: точные IEEE-биты обоих
// игроков и хронологические trim/wipe/reseal. Никаких записей в состояние GD.
namespace skkbot2::seam_journal {
enum class Kind : uint32_t { Sample = 1, Seal = 2, Trim = 3, Wipe = 4, End = 5, Gap = 6 };
struct Player {
    float x = 0, y = 0, rotation = 0;
    uint32_t held = 0;
    double vy = 0, vyb = 0;
    uint32_t visual = 0, ring = 0, flags = 0, reserved = 0;
};
struct Event {
    Kind kind = Kind::Sample;
    int32_t tick = 0;
    uint32_t traceRow = 0, active = 0;
    double time = 0;
    Player p1, p2;
};
inline constexpr size_t kRowBytes = 120;
inline auto encode(Event const& event) {
    std::array<uint8_t, kRowBytes> bytes{};
    size_t offset = 0;
    auto put = [&](auto value) {
        // Поддерживаемые Geode платформы little-endian; скалярные IEEE-типы.
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    put(static_cast<uint32_t>(event.kind)); put(event.tick);
    put(event.traceRow); put(event.active); put(event.time);
    for (auto const* p : { &event.p1, &event.p2 }) {
        put(p->x); put(p->y); put(p->rotation); put(p->held);
        put(p->vy); put(p->vyb); put(p->visual); put(p->ring);
        put(p->flags); put(p->reserved);
    }
    return bytes;
}
}

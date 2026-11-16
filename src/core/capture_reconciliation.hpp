#pragma once

namespace skkbot2::capture_reconciliation {

// Только игровой поток. Qualified вызов GD из checkpoint_system.cpp снова
// входит в Geode-hook: блокируем его capture-часть, но не vanilla-эффект.
// Не поле ReplayEngine и не кадровое окно; scope заканчивается сразу после
// внутреннего handleButton. Inline-переменная общая для обоих translation unit.
inline unsigned depth = 0;

inline bool active() noexcept { return depth != 0; }

class Scope {
public:
    Scope() noexcept { ++depth; }
    ~Scope() noexcept { --depth; }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
};

} // namespace skkbot2::capture_reconciliation

#pragma once

namespace skkbot2::capture_reconciliation {

inline unsigned depth = 0;

inline bool active() noexcept { return depth != 0; }

class Scope {
public:
    Scope() noexcept { ++depth; }
    ~Scope() noexcept { --depth; }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
};

}

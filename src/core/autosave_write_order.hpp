#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace skkbot2::autosave {

class SnapshotWriteOrder {
public:
    uint64_t issue() { return ++m_issued; }

    template <class Write>
    bool run(uint64_t revision, std::string_view destination, Write&& write) {
        std::lock_guard lock(m_mutex);
        auto& committed = m_committed[std::string(destination)];
        if (revision <= committed) return false;
        if (std::forward<Write>(write)()) committed = revision;
        return true;
    }

private:
    std::atomic<uint64_t> m_issued{0};
    std::mutex m_mutex;
    std::unordered_map<std::string, uint64_t> m_committed;
};

}

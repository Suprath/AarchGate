#pragma once

#ifdef APEX_HAS_ICEORYX

#include <optional>
#include "iceoryx_posh/popo/subscriber.hpp"
#include "iceoryx_posh/capro/service_description.hpp"

namespace apex::memory {

template <typename T>
class ShmFabric {
public:
    ShmFabric(const char* service, const char* instance, const char* event) noexcept
        : subscriber_({iox::capro::IdString_t{iox::TruncateToCapacity, service},
                       iox::capro::IdString_t{iox::TruncateToCapacity, instance},
                       iox::capro::IdString_t{iox::TruncateToCapacity, event}})
    {}

    // Wait-free polling: returns nullopt immediately if no data
    std::optional<iox::popo::Sample<const T>> poll() noexcept {
        auto result = subscriber_.take();
        if (result.has_error()) return std::nullopt;
        return std::optional<iox::popo::Sample<const T>>(std::move(result.value()));
    }

private:
    iox::popo::Subscriber<T> subscriber_;
};

} // namespace apex::memory

#endif // APEX_HAS_ICEORYX

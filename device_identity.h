#pragma once

#include <string>
#include <string_view>

namespace ss {

// Legacy deterministic helper kept only for migration tests. New installs do
// not read hardware identifiers.
[[nodiscard]] std::string DeviceBindingIdFromMachineGuid(std::string_view machineGuid);
// Returns a random per-installation UUID persisted under LocalAppData. It can
// be reset by uninstalling/removing Sonalis data and is not a hardware fingerprint.
[[nodiscard]] std::string StableDeviceBindingId();

}  // namespace ss

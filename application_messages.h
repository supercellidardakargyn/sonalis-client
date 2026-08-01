#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ss {

inline constexpr UINT kMessageTrayCallback = WM_APP + 41U;
inline constexpr UINT kMessageShowApplication = WM_APP + 42U;
inline constexpr UINT kMessageExitApplication = WM_APP + 43U;
inline constexpr UINT kMessageNamedPipeDeepLink = WM_APP + 44U;
inline constexpr UINT kTrayIconId = 1U;
inline constexpr ULONG_PTR kDeepLinkCopyDataTag = 0x534F4E41U;  // "SONA"

}  // namespace ss

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <sddl.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "ui.h"
#include "application_messages.h"
#include "diagnostics.h"
#include "resources.h"
#include "localization.h"
#include "performance.h"
#include "theme_engine.h"
#include "win_helpers.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace {

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Device> gDevice;
ComPtr<ID3D11DeviceContext> gContext;
ComPtr<IDXGISwapChain> gSwapChain;
ComPtr<IDXGISwapChain2> gSwapChain2;
HANDLE gFrameLatencyWaitable{};
ComPtr<ID3D11RenderTargetView> gRenderTarget;
ComPtr<ID3D11ShaderResourceView> gLogoTexture;
float gRequestedDpiScale{1.0F};
bool gDpiChangePending{false};
bool gRendererRecoveryPending{false};
bool gUsingWarpRenderer{false};
ss::AppUi* gAppUi{};
HICON gApplicationIcon{};
bool gTrayIconAdded{false};
bool gExitRequested{false};
bool gCloseHintShown{false};
UINT gTaskbarCreatedMessage{};
std::wstring gDeferredDeepLink;

void ApplyNativeWindowTheme(const HWND window, const ss::UiTheme theme) noexcept {
    if (window == nullptr) return;

    // Keep the native Windows caption controls, accessibility and snap layouts,
    // but make the non-client area part of the active Sonalis theme. Numeric
    // attribute IDs let the same binary run on older Windows 10 builds that do
    // not expose the newer DWM enum names in their SDK.
    constexpr DWORD kUseImmersiveDarkMode = 20U;
    constexpr DWORD kWindowCornerPreference = 33U;
    constexpr DWORD kBorderColor = 34U;
    constexpr DWORD kCaptionColor = 35U;
    constexpr DWORD kTextColor = 36U;
    constexpr DWORD kRoundCornerPreference = 2U;

    const bool lightTheme = theme == ss::UiTheme::AuroraLight;
    const BOOL darkMode = lightTheme ? FALSE : TRUE;
    const COLORREF caption = lightTheme ? RGB(244, 247, 252) : RGB(7, 11, 22);
    const COLORREF border = lightTheme ? RGB(202, 211, 229) : RGB(31, 41, 68);
    const COLORREF text = lightTheme ? RGB(18, 25, 40) : RGB(238, 243, 255);
    const DWORD corners = kRoundCornerPreference;

    (void)DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    (void)DwmSetWindowAttribute(window, kCaptionColor, &caption, sizeof(caption));
    (void)DwmSetWindowAttribute(window, kBorderColor, &border, sizeof(border));
    (void)DwmSetWindowAttribute(window, kTextColor, &text, sizeof(text));
    (void)DwmSetWindowAttribute(window, kWindowCornerPreference, &corners, sizeof(corners));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

enum class RendererLifecycle : unsigned char {
    Booting,
    CreatingDevice,
    CreatingFonts,
    Ready,
    Hidden,
    DeviceLost,
    Recovering,
    SafeMode,
    FatalError,
};

std::atomic<RendererLifecycle> gRendererLifecycle{RendererLifecycle::Booting};

const char* RendererLifecycleName(const RendererLifecycle state) noexcept {
    switch (state) {
        case RendererLifecycle::Booting: return "booting";
        case RendererLifecycle::CreatingDevice: return "creating-device";
        case RendererLifecycle::CreatingFonts: return "creating-fonts";
        case RendererLifecycle::Ready: return "ready";
        case RendererLifecycle::Hidden: return "hidden";
        case RendererLifecycle::DeviceLost: return "device-lost";
        case RendererLifecycle::Recovering: return "recovering";
        case RendererLifecycle::SafeMode: return "safe-mode";
        case RendererLifecycle::FatalError: return "fatal-error";
    }
    return "unknown";
}

void SetRendererLifecycle(const RendererLifecycle state) {
    if (gRendererLifecycle.exchange(state) == state) return;
    ss::DiagnosticLog("renderer-state", RendererLifecycleName(state));
}

constexpr UINT kTrayOpenCommand = 40'001U;
constexpr UINT kTrayExitCommand = 40'002U;
constexpr UINT kTrayMicrophoneCommand = 40'003U;
// Global namespace makes the instance lock span every interactive Windows
// session on this machine, not just the current signed-in desktop.
constexpr wchar_t kSingleInstanceMutex[] = L"Global\\Sonalis.Desktop.MachineInstance.v1";
constexpr wchar_t kDeepLinkPipe[] = L"\\\\.\\pipe\\Sonalis.Desktop.Command.v1";

#if defined(SONALIS_REQUIRE_AUTHENTICODE)
bool HasPinnedAuthenticodeSigner(const wchar_t* path) noexcept {
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0;
    DWORD content = 0;
    DWORD format = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format,
                          &store, &message, nullptr)) return false;
    bool matched = false;
    DWORD signerBytes = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerBytes) && signerBytes > 0) {
        std::vector<std::uint8_t> storage(signerBytes);
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, storage.data(), &signerBytes)) {
            const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(storage.data());
            CERT_INFO certificateInfo{};
            certificateInfo.Issuer = signer->Issuer;
            certificateInfo.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT certificate = CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
                &certificateInfo, nullptr);
            if (certificate != nullptr) {
                std::array<std::uint8_t, 32> digest{};
                DWORD digestBytes = static_cast<DWORD>(digest.size());
                if (CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                                          certificate->pbCertEncoded, certificate->cbCertEncoded,
                                          digest.data(), &digestBytes)
                    && digestBytes == static_cast<DWORD>(digest.size())) {
                    constexpr std::string_view expected = SONALIS_AUTHENTICODE_CERT_SHA256;
                    matched = expected.size() == digest.size() * 2;
                    auto nibble = [](const char character) noexcept -> int {
                        if (character >= '0' && character <= '9') return character - '0';
                        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                        return -1;
                    };
                    for (std::size_t index = 0; matched && index < digest.size(); ++index) {
                        const int high = nibble(expected[index * 2]);
                        const int low = nibble(expected[index * 2 + 1]);
                        matched = high >= 0 && low >= 0
                            && digest[index] == static_cast<std::uint8_t>((high << 4) | low);
                    }
                }
                CertFreeCertificateContext(certificate);
            }
        }
    }
    if (message != nullptr) CryptMsgClose(message);
    if (store != nullptr) CertCloseStore(store, 0);
    return matched;
}

bool VerifyAuthenticodeSelf() noexcept {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return false;
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path;
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    return result == ERROR_SUCCESS && HasPinnedAuthenticodeSigner(path);
}
#endif

std::vector<std::wstring> CommandLineArguments() {
    std::vector<std::wstring> arguments;
    const wchar_t* cursor = GetCommandLineW();
    if (cursor == nullptr) return arguments;
    if (*cursor == L'"') {
        ++cursor;
        while (*cursor != L'\0' && *cursor != L'"') ++cursor;
        if (*cursor == L'"') ++cursor;
    } else {
        while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') ++cursor;
    }
    while (*cursor != L'\0') {
        while (*cursor == L' ' || *cursor == L'\t') ++cursor;
        if (*cursor == L'\0') break;
        const bool quoted = *cursor == L'"';
        if (quoted) ++cursor;
        const wchar_t* start = cursor;
        while (*cursor != L'\0' && (quoted ? *cursor != L'"' : (*cursor != L' ' && *cursor != L'\t'))) ++cursor;
        arguments.emplace_back(start, cursor);
        if (quoted && *cursor == L'"') ++cursor;
    }
    return arguments;
}

bool HasArgument(const std::vector<std::wstring>& arguments, const std::wstring_view expected) {
    return std::ranges::find(arguments, expected) != arguments.end();
}

void ApplyDeepLink(const std::wstring_view deepLink) {
    if (deepLink.empty()) return;
    if (gAppUi == nullptr) {
        gDeferredDeepLink.assign(deepLink);
        return;
    }
    constexpr std::wstring_view invitePrefix = L"sonalis://join/";
    constexpr std::wstring_view roomPrefix = L"sonalis://room/";
    if (deepLink.starts_with(invitePrefix)) {
        gAppUi->SetPendingInvite(ss::WideToUtf8(std::wstring(deepLink.substr(invitePrefix.size()))));
    } else if (deepLink.starts_with(roomPrefix)) {
        gAppUi->SetPendingRoom(ss::WideToUtf8(std::wstring(deepLink.substr(roomPrefix.size()))));
    }
}

bool AddTrayIcon(const HWND window) {
    if (window == nullptr) return false;
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = window;
    notification.uID = ss::kTrayIconId;
    notification.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notification.uCallbackMessage = ss::kMessageTrayCallback;
    notification.hIcon = gApplicationIcon;
    lstrcpynW(notification.szTip, L"Sonalis", ARRAYSIZE(notification.szTip));
    gTrayIconAdded = ss::DynamicShellNotify(gTrayIconAdded ? NIM_MODIFY : NIM_ADD, &notification);
    if (gTrayIconAdded) {
        notification.uVersion = NOTIFYICON_VERSION_4;
        ss::DynamicShellNotify(NIM_SETVERSION, &notification);
    }
    return gTrayIconAdded;
}

void RemoveTrayIcon(const HWND window) noexcept {
    if (!gTrayIconAdded || window == nullptr) return;
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = window;
    notification.uID = ss::kTrayIconId;
    ss::DynamicShellNotify(NIM_DELETE, &notification);
    gTrayIconAdded = false;
}

void ShowApplicationWindow(const HWND window) {
    if (window == nullptr) return;
    ShowWindow(window, IsIconic(window) != FALSE ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window);
    FLASHWINFO flash{sizeof(flash), window, FLASHW_TRAY, 2, 0};
    FlashWindowEx(&flash);
}

void ShowCloseToTrayHint(const HWND window) {
    if (gCloseHintShown || !gTrayIconAdded) return;
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = window;
    notification.uID = ss::kTrayIconId;
    notification.uFlags = NIF_INFO;
    lstrcpynW(notification.szInfoTitle, L"Sonalis arka planda calisiyor", ARRAYSIZE(notification.szInfoTitle));
    lstrcpynW(notification.szInfo, L"Pencere kapatildi; ses ve mesaj baglantilari bildirim alaninda suruyor.",
              ARRAYSIZE(notification.szInfo));
    notification.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    ss::DynamicShellNotify(NIM_MODIFY, &notification);
    gCloseHintShown = true;
}

void ShowTrayMenu(const HWND window) {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayOpenCommand, L"Sonalis'i ac");
    if (gAppUi != nullptr) {
        const std::wstring microphone = gAppUi->IsMicrophoneMuted() ? L"Mikrofonu ac" : L"Mikrofonu kapat";
        AppendMenuW(menu, MF_STRING, kTrayMicrophoneCommand, microphone.c_str());
        const std::string activeRoom = gAppUi->ActiveRoomName();
        if (!activeRoom.empty()) {
            const std::wstring label = L"Aktif oda: " + ss::Utf8ToWide(activeRoom);
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, label.c_str());
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"Sonalis'ten cik");
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (command != 0U) PostMessageW(window, WM_COMMAND, command, 0);
    PostMessageW(window, WM_NULL, 0, 0);
}

bool SendDeepLinkToPipe(const std::wstring& deepLink) {
    if (deepLink.empty() || deepLink.size() > 2'047U) return deepLink.empty();
    for (unsigned attempt = 0; attempt < 40U; ++attempt) {
        const HANDLE pipe = CreateFileW(kDeepLinkPipe, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            const DWORD payloadBytes = static_cast<DWORD>(deepLink.size() * sizeof(wchar_t));
            DWORD written = 0;
            const BOOL ok = WriteFile(pipe, &payloadBytes, sizeof(payloadBytes), &written, nullptr)
                && written == sizeof(payloadBytes)
                && WriteFile(pipe, deepLink.data(), payloadBytes, &written, nullptr)
                && written == payloadBytes;
            FlushFileBuffers(pipe);
            CloseHandle(pipe);
            return ok != FALSE;
        }
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        WaitNamedPipeW(kDeepLinkPipe, 50);
    }
    return false;
}

void NamedPipeLoop(const std::stop_token stopToken, const HWND window) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    while (!stopToken.stop_requested()) {
        const HANDLE pipe = CreateNamedPipeW(
            kDeepLinkPipe, PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 0, 4'096, 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) break;
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !stopToken.stop_requested()) {
            DWORD payloadBytes = 0;
            DWORD read = 0;
            if (ReadFile(pipe, &payloadBytes, sizeof(payloadBytes), &read, nullptr)
                && read == sizeof(payloadBytes) && payloadBytes >= sizeof(wchar_t)
                && payloadBytes <= 4'094U && payloadBytes % sizeof(wchar_t) == 0U) {
                auto* deepLink = new (std::nothrow) std::wstring(payloadBytes / sizeof(wchar_t), L'\0');
                if (deepLink != nullptr) {
                    if (ReadFile(pipe, deepLink->data(), payloadBytes, &read, nullptr) && read == payloadBytes) {
                        PostMessageW(window, ss::kMessageNamedPipeDeepLink, 0, reinterpret_cast<LPARAM>(deepLink));
                    } else {
                        delete deepLink;
                    }
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    LocalFree(descriptor);
}

bool ForwardToExistingInstance(const std::wstring& deepLink) {
    HWND existing = nullptr;
    for (unsigned attempt = 0; attempt < 40U && existing == nullptr; ++attempt) {
        existing = FindWindowW(L"SonalisWindow", nullptr);
        if (existing == nullptr) Sleep(25);
    }
    if (existing == nullptr) return false;
    AllowSetForegroundWindow(ASFW_ANY);
    if (!deepLink.empty()) SendDeepLinkToPipe(deepLink);
    PostMessageW(existing, ss::kMessageShowApplication, 0, 0);
    return true;
}

bool CreateRenderTarget() {
    if (!gSwapChain || !gDevice) return false;
    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        const HRESULT result = gDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &gRenderTarget);
        if (FAILED(result)) {
            ss::DiagnosticLog("renderer", "render-target-hr=" + std::to_string(static_cast<unsigned long>(result)));
        }
    }
    return gRenderTarget != nullptr;
}

void CleanupRenderTarget() { gRenderTarget.Reset(); }
void CleanupDeviceD3D();

bool CreateDeviceD3D(const HWND window, const bool forceWarp = false) {
    CleanupDeviceD3D();
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    auto create = [&](const D3D_DRIVER_TYPE driver) {
        return D3D11CreateDeviceAndSwapChain(
            nullptr, driver, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &description, &gSwapChain, &gDevice, &selected, &gContext);
    };
    HRESULT result = create(forceWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
    gUsingWarpRenderer = forceWarp;
    if (FAILED(result) && !forceWarp) {
        ss::DiagnosticLog("renderer", "hardware-device-hr=" + std::to_string(static_cast<unsigned long>(result)));
        CleanupDeviceD3D();
        result = create(D3D_DRIVER_TYPE_WARP);
        gUsingWarpRenderer = SUCCEEDED(result);
    }
    if (FAILED(result)) {
        ss::DiagnosticLog("renderer", "device-create-hr=" + std::to_string(static_cast<unsigned long>(result)));
        CleanupDeviceD3D();
        return false;
    }
    if (SUCCEEDED(gSwapChain.As(&gSwapChain2))) {
        gSwapChain2->SetMaximumFrameLatency(1);
        gFrameLatencyWaitable = gSwapChain2->GetFrameLatencyWaitableObject();
    }
    if (!CreateRenderTarget()) {
        CleanupDeviceD3D();
        return false;
    }
    ss::DiagnosticLog("renderer", gUsingWarpRenderer ? "device-ready-warp" : "device-ready-hardware");
    return true;
}

bool LoadLogoTexture(const HINSTANCE instance) {
    constexpr UINT width = 64;
    constexpr UINT height = 64;
    const HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SONALIS), IMAGE_ICON,
                                                      width, height, LR_DEFAULTCOLOR));
    if (icon == nullptr) return false;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const HDC memoryDc = bitmap != nullptr ? CreateCompatibleDC(nullptr) : nullptr;
    if (bitmap == nullptr || memoryDc == nullptr || pixels == nullptr) {
        if (memoryDc != nullptr) DeleteDC(memoryDc);
        if (bitmap != nullptr) DeleteObject(bitmap);
        DestroyIcon(icon);
        return false;
    }
    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    PatBlt(memoryDc, 0, 0, static_cast<int>(width), static_cast<int>(height), BLACKNESS);
    const BOOL drawn = DrawIconEx(memoryDc, 0, 0, icon, static_cast<int>(width), static_cast<int>(height), 0, nullptr, DI_NORMAL);
    SelectObject(memoryDc, previous);
    DeleteDC(memoryDc);
    DestroyIcon(icon);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels;
    initial.SysMemPitch = width * 4;
    ComPtr<ID3D11Texture2D> texture;
    const bool created = drawn != FALSE && SUCCEEDED(gDevice->CreateTexture2D(&description, &initial, &texture))
        && SUCCEEDED(gDevice->CreateShaderResourceView(texture.Get(), nullptr, &gLogoTexture));
    DeleteObject(bitmap);
    return created;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    gSwapChain.Reset();
    gSwapChain2.Reset();
    gFrameLatencyWaitable = nullptr;
    gLogoTexture.Reset();
    gContext.Reset();
    gDevice.Reset();
}

float WindowDpiScale(const HWND window) {
    const UINT dpi = window != nullptr ? GetDpiForWindow(window) : GetDpiForSystem();
    // 16K workstations commonly use 300-400% per-monitor scaling.  Keep the
    // logical UI readable without forcing a second oversized font atlas.
    return std::clamp(static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F, 1.0F, 4.0F);
}

bool ConfigureFontsAndStyle(const float dpiScale,
                            const ss::Language language,
                            const ss::UiTheme requestedTheme,
                            const ss::ResourceProfile requestedProfile,
                            const float requestedTextScale,
                            const int requestedDensity,
                            const bool requestedHighContrast,
                            const int requestedColorVisionMode,
                            const float requestedCustomAccentR,
                            const float requestedCustomAccentG,
                            const float requestedCustomAccentB,
                            const bool safeUi = false) {
    const ss::UiTheme theme = safeUi ? ss::UiTheme::Classic : requestedTheme;
    const ss::ResourceProfile profile = safeUi ? ss::ResourceProfile::Economy : requestedProfile;
    const float textScale = safeUi ? 1.0F : requestedTextScale;
    const int density = safeUi ? 1 : requestedDensity;
    const bool highContrast = safeUi || requestedHighContrast;
    const int colorVisionMode = safeUi ? 0 : requestedColorVisionMode;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static ImVector<ImWchar> glyphRanges;
    glyphRanges.clear();
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (std::size_t index = 0; index < static_cast<std::size_t>(ss::TextId::Count); ++index) {
        builder.AddText(ss::Translate(language, static_cast<ss::TextId>(index)));
    }
    // The selector always shows every language in its own script; this is a
    // very small set compared with loading every language catalogue.
    for (const auto& option : ss::SupportedLanguages()) builder.AddText(ss::LanguageDisplayName(option.language));
    builder.BuildRanges(&glyphRanges);
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory));
    const wchar_t* regularFontName = L"segoeui.ttf";
    const wchar_t* semiboldFontName = L"seguisb.ttf";
    if (language == ss::Language::Japanese) {
        regularFontName = L"YuGothM.ttc";
        semiboldFontName = L"YuGothB.ttc";
    } else if (language == ss::Language::Korean) {
        regularFontName = L"malgun.ttf";
        semiboldFontName = L"malgunbd.ttf";
    } else if (language == ss::Language::SimplifiedChinese) {
        regularFontName = L"msyh.ttc";
        semiboldFontName = L"msyhbd.ttc";
    }
    const float baseFontPixels = 16.5F * dpiScale * std::clamp(textScale, 0.85F, 1.35F);
    const auto loadWindowsFont = [&](const wchar_t* fontName, const char* debugName) -> ImFont* {
        const std::wstring fontPath = std::wstring(windowsDirectory) + L"\\Fonts\\" + fontName;
        const HANDLE fontFile = CreateFileW(fontPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fontFile == INVALID_HANDLE_VALUE) return nullptr;

        ImFont* result = nullptr;
        LARGE_INTEGER fileSize{};
        if (GetFileSizeEx(fontFile, &fileSize) && fileSize.QuadPart > 0
            && fileSize.QuadPart <= 32LL * 1024LL * 1024LL) {
            void* fontData = IM_ALLOC(static_cast<std::size_t>(fileSize.QuadPart));
            DWORD bytesRead = 0;
            if (fontData != nullptr
                && ReadFile(fontFile, fontData, static_cast<DWORD>(fileSize.QuadPart), &bytesRead, nullptr)
                && bytesRead == static_cast<DWORD>(fileSize.QuadPart)) {
                ImFontConfig fontConfig{};
                fontConfig.OversampleH = 1;
                fontConfig.OversampleV = 1;
                fontConfig.PixelSnapH = true;
                strncpy_s(fontConfig.Name, debugName, _TRUNCATE);
                result = io.Fonts->AddFontFromMemoryTTF(fontData, bytesRead, baseFontPixels,
                                                        &fontConfig, glyphRanges.Data);
                if (result == nullptr) IM_FREE(fontData);
            } else if (fontData != nullptr) {
                IM_FREE(fontData);
            }
        }
        CloseHandle(fontFile);
        return result;
    };

    // Both faces share one tightly subsetted atlas. This provides a real
    // typographic hierarchy without adding another GPU texture or loading the
    // full Unicode ranges into memory.
    ImFont* loadedFont = loadWindowsFont(regularFontName, "Sonalis Regular");
    if (loadedFont == nullptr) {
        ImFontConfig fallback{};
        fallback.SizePixels = 16.0F * dpiScale * std::clamp(textScale, 0.85F, 1.35F);
        strncpy_s(fallback.Name, "Sonalis Fallback", _TRUNCATE);
        loadedFont = io.Fonts->AddFontDefault(&fallback);
    }
    if (loadedFont == nullptr || io.Fonts->Fonts.Size == 0) {
        ss::DiagnosticLog("renderer", "font-atlas-build-failed");
        return false;
    }
    if (!safeUi) {
        (void)loadWindowsFont(semiboldFontName, "Sonalis Semibold");
    }
    io.FontDefault = loadedFont;
    ImGuiStyle& style = ImGui::GetStyle();
    // ConfigureFontsAndStyle is also called after a DPI/theme change.  Reset
    // the style before ScaleAllSizes so switching monitors never compounds the
    // previous scale and makes controls grow on every transition.
    style = ImGuiStyle{};
    ss::ApplyClientTheme(style, theme, profile, ss::IsRightToLeft(language), dpiScale,
                         textScale, density, highContrast, colorVisionMode,
                         requestedCustomAccentR, requestedCustomAccentG, requestedCustomAccentB);
    ss::DiagnosticLog("renderer", safeUi ? "font-ready-safe" : "font-ready-system");
    return true;
}

LRESULT WINAPI WindowProc(const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    if (gTaskbarCreatedMessage != 0U && message == gTaskbarCreatedMessage) {
        gTrayIconAdded = false;
        AddTrayIcon(window);
        return 0;
    }
    if (message == ss::kMessageShowApplication) {
        ShowApplicationWindow(window);
        return 0;
    }
    if (message == ss::kMessageExitApplication) {
        gExitRequested = true;
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == ss::kMessageNamedPipeDeepLink) {
        std::unique_ptr<std::wstring> deepLink(reinterpret_cast<std::wstring*>(lParam));
        if (deepLink) ApplyDeepLink(*deepLink);
        ShowApplicationWindow(window);
        return 0;
    }
    if (message == ss::kMessageTrayCallback) {
        const UINT event = LOWORD(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) {
            ShowApplicationWindow(window);
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            ShowTrayMenu(window);
        }
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return TRUE;
    switch (message) {
        case WM_SIZE:
            if (gDevice && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                const HRESULT result = gSwapChain->ResizeBuffers(
                    0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)),
                    DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
                if (FAILED(result) || !CreateRenderTarget()) gRendererRecoveryPending = true;
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0U) == SC_KEYMENU) return 0;
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == kTrayOpenCommand) {
                ShowApplicationWindow(window);
                return 0;
            }
            if (LOWORD(wParam) == kTrayExitCommand) {
                gExitRequested = true;
                PostMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            if (LOWORD(wParam) == kTrayMicrophoneCommand && gAppUi != nullptr) {
                gAppUi->ToggleMicrophoneMuted();
                return 0;
            }
            break;
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data != nullptr && data->dwData == ss::kDeepLinkCopyDataTag && data->lpData != nullptr
                && data->cbData >= sizeof(wchar_t) && data->cbData <= 4'096U
                && data->cbData % sizeof(wchar_t) == 0U) {
                const auto* value = static_cast<const wchar_t*>(data->lpData);
                std::size_t length = data->cbData / sizeof(wchar_t);
                if (length > 0U && value[length - 1U] == L'\0') --length;
                ApplyDeepLink(std::wstring_view(value, length));
                ShowApplicationWindow(window);
                return TRUE;
            }
            return FALSE;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(window);
            info->ptMinTrackSize.x = MulDiv(960, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
            info->ptMinTrackSize.y = MulDiv(640, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            gRequestedDpiScale = static_cast<float>(HIWORD(wParam)) / 96.0F;
            gDpiChangePending = true;
            return 0;
        }
        case WM_CLOSE:
            if (gAppUi != nullptr && gAppUi->IsStartupUpdateGateActive()) {
                gExitRequested = true;
                PostQuitMessage(0);
                return 0;
            }
            if (!gExitRequested) {
                ShowWindow(window, SW_HIDE);
                ShowCloseToTrayHint(window);
                return 0;
            }
            // Keep the HWND alive until the main loop has persisted the last
            // normal window placement and shut down audio/network cleanly.
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon(window);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto startupBegan = std::chrono::steady_clock::now();
#if defined(SONALIS_REQUIRE_AUTHENTICODE)
    if (!VerifyAuthenticodeSelf()) {
        MessageBoxW(nullptr, L"Sonalis imzasi dogrulanamadi. Uygulamayi resmi kaynaktan yeniden indirin.",
                    L"Sonalis guvenlik hatasi", MB_OK | MB_ICONERROR);
        return 2;
    }
#endif
    const std::vector<std::wstring> arguments = CommandLineArguments();
    const bool startInBackground = HasArgument(arguments, L"--background");
    const bool safeUi = HasArgument(arguments, L"--safe-ui");
    const bool disableGpu = HasArgument(arguments, L"--disable-gpu");
    if (HasArgument(arguments, L"--reset-ui")) {
        std::string resetError;
        if (!ss::SettingsStore{}.ResetUi(resetError)) {
            ss::DiagnosticLog("startup", "reset-ui-failed=" + resetError);
        }
    }
    std::wstring initialDeepLink;
    for (const auto& argument : arguments) {
        if (argument.starts_with(L"sonalis://")) {
            initialDeepLink = argument;
            break;
        }
    }
    ss::DiagnosticLog("startup", safeUi ? "begin-safe-ui" : (disableGpu ? "begin-warp" : "begin"));
    const HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (singleInstanceMutex == nullptr) {
        // An instance owned by another Windows user/session can deny opening
        // the existing mutex. Treat that as an active machine-wide instance
        // instead of starting a second process.
        return GetLastError() == ERROR_ACCESS_DENIED ? 0 : 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ForwardToExistingInstance(initialDeepLink);
        CloseHandle(singleInstanceMutex);
        return 0;
    }
    // Resource use is controlled by bounded queues/caches and event-driven
    // rendering. A hard working-set quota can evict the DX11 font atlas and
    // leave a logo-only window, so it is deliberately not used.
    ImGui_ImplWin32_EnableDpiAwareness();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    gApplicationIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SONALIS), IMAGE_ICON,
                                                      0, 0, LR_DEFAULTSIZE));
    gTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW windowClass{sizeof(windowClass), CS_CLASSDC, WindowProc, 0, 0, instance, gApplicationIcon,
                            LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"SonalisWindow", gApplicationIcon};
    if (RegisterClassExW(&windowClass) == 0) {
        ss::DiagnosticLog("startup", "register-window-class-failed");
        if (SUCCEEDED(comResult)) CoUninitialize();
        CloseHandle(singleInstanceMutex);
        return 1;
    }
    const ss::AppSettings startupSettings = ss::SettingsStore{}.Load();
    const float startupScale = WindowDpiScale(nullptr);
    int windowX = startupSettings.hasWindowPlacement ? startupSettings.windowX : 100;
    int windowY = startupSettings.hasWindowPlacement ? startupSettings.windowY : 100;
    int windowWidth = startupSettings.hasWindowPlacement ? startupSettings.windowWidth
                                                          : static_cast<int>(1240.0F * startupScale);
    int windowHeight = startupSettings.hasWindowPlacement ? startupSettings.windowHeight
                                                           : static_cast<int>(780.0F * startupScale);
    RECT proposed{windowX, windowY, windowX + windowWidth, windowY + windowHeight};
    if (MonitorFromRect(&proposed, MONITOR_DEFAULTTONULL) == nullptr) {
        windowX = 100;
        windowY = 100;
    }
    const HWND window = CreateWindowW(windowClass.lpszClassName, L"Sonalis", WS_OVERLAPPEDWINDOW,
                                      windowX, windowY, windowWidth, windowHeight,
                                      nullptr, nullptr, instance, nullptr);
    ApplyNativeWindowTheme(window, safeUi ? ss::UiTheme::Classic : startupSettings.uiTheme);
    SetRendererLifecycle(RendererLifecycle::CreatingDevice);
    if (window == nullptr || !CreateDeviceD3D(window, disableGpu)) {
        SetRendererLifecycle(RendererLifecycle::FatalError);
        ss::DiagnosticLog("startup", window == nullptr ? "window-create-failed" : "renderer-create-failed");
        MessageBoxW(nullptr,
                    L"Sonalis grafik arayuzu baslatilamadi. Uygulamayi --safe-ui veya --disable-gpu ile acmayi deneyin.",
                    L"Sonalis arayuz hatasi", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D();
        if (window != nullptr) DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (SUCCEEDED(comResult)) CoUninitialize();
        CloseHandle(singleInstanceMutex);
        return 1;
    }

    std::jthread namedPipeThread([window](const std::stop_token token) { NamedPipeLoop(token, window); });

    AddTrayIcon(window);
    ShowWindow(window, startInBackground ? SW_HIDE
                                         : (startupSettings.windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT));
    UpdateWindow(window);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    SetRendererLifecycle(RendererLifecycle::CreatingFonts);
    const bool fontsReady = ConfigureFontsAndStyle(WindowDpiScale(window),
                                                   ss::ParseLanguage(startupSettings.language),
                                                   startupSettings.uiTheme,
                                                   startupSettings.resourceProfile,
                                                   startupSettings.textScale,
                                                   startupSettings.uiDensity,
                                                   startupSettings.highContrast,
                                                   startupSettings.colorVisionMode,
                                                   startupSettings.customAccentR,
                                                   startupSettings.customAccentG,
                                                   startupSettings.customAccentB,
                                                   safeUi);
    const bool win32Ready = ImGui_ImplWin32_Init(window);
    const bool dx11Ready = win32Ready && ImGui_ImplDX11_Init(gDevice.Get(), gContext.Get());
    const bool deviceObjectsReady = dx11Ready && fontsReady && ImGui_ImplDX11_CreateDeviceObjects();
    if (!deviceObjectsReady) {
        SetRendererLifecycle(RendererLifecycle::FatalError);
        ss::DiagnosticLog("startup", "imgui-backend-initialization-failed");
        MessageBoxW(window,
                    L"Sonalis arayuz kaynaklari olusturulamadi. --safe-ui secenegiyle yeniden acmayi deneyin.",
                    L"Sonalis arayuz hatasi", MB_OK | MB_ICONERROR);
        if (dx11Ready) ImGui_ImplDX11_Shutdown();
        if (win32Ready) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (SUCCEEDED(comResult)) CoUninitialize();
        CloseHandle(singleInstanceMutex);
        return 1;
    }
    LoadLogoTexture(instance);

    ss::AppUi app;
    gAppUi = &app;
    app.SetWindowHandle(window);
    app.SetLogoTexture(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(gLogoTexture.Get())));
    if (gUsingWarpRenderer) {
        app.SetStartupWarning("Grafik hizlandirma kullanilamadi; guvenli renderer etkin.");
    }
    SetRendererLifecycle(gUsingWarpRenderer ? RendererLifecycle::SafeMode : RendererLifecycle::Ready);
    ApplyDeepLink(initialDeepLink);
    if (!gDeferredDeepLink.empty()) {
        const std::wstring deferred = std::exchange(gDeferredDeepLink, {});
        ApplyDeepLink(deferred);
    }
    app.Initialize();
    if (std::chrono::steady_clock::now() - startupBegan > std::chrono::seconds(5)) {
        ss::DiagnosticLog("startup-watchdog", "interactive-screen-over-5s");
        app.SetStartupWarning("Sonalis beklenenden yavas basladi. Tanilama kaydi olusturuldu.");
    }
    bool done = false;
    bool firstFramePresented = false;
    while (!done) {
        const bool visible = IsWindowVisible(window) != FALSE;
        if (!visible || IsIconic(window)) {
            SetRendererLifecycle(RendererLifecycle::Hidden);
        } else if (gRendererLifecycle.load() == RendererLifecycle::Hidden) {
            SetRendererLifecycle(gUsingWarpRenderer ? RendererLifecycle::SafeMode : RendererLifecycle::Ready);
        }
        const bool focused = visible && GetForegroundWindow() == window;
        const DWORD waitMilliseconds = app.RefreshIntervalMs(focused, visible);
        const HANDLE redraw = app.RedrawEvent();
        const DWORD handleCount = redraw != nullptr ? 1U : 0U;
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(handleCount, handleCount ? &redraw : nullptr,
                                                             waitMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        bool inputArrived = waitResult == WAIT_OBJECT_0 + handleCount;
        bool redrawRequested = handleCount != 0 && waitResult == WAIT_OBJECT_0;
        const bool timerElapsed = waitResult == WAIT_TIMEOUT;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            inputArrived = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) continue;
        app.Tick();
        const bool renderVisible = IsWindowVisible(window) != FALSE;
        if (!renderVisible || IsIconic(window)) {
            app.ConsumeRedraw();
            continue;
        }
        if (gRendererRecoveryPending) {
            SetRendererLifecycle(RendererLifecycle::Recovering);
            ss::DiagnosticLog("renderer", "recovery-started");
            app.SetLogoTexture(0);
            ImGui_ImplDX11_Shutdown();
            const bool deviceReady = CreateDeviceD3D(window, disableGpu);
            const bool fontReady = deviceReady && ConfigureFontsAndStyle(
                WindowDpiScale(window), app.ActiveLanguage(), app.ActiveTheme(),
                app.ActiveResourceProfile(), app.ActiveTextScale(), app.ActiveUiDensity(),
                app.ActiveHighContrast(), app.ActiveColorVisionMode(),
                app.ActiveCustomAccentR(), app.ActiveCustomAccentG(), app.ActiveCustomAccentB(), safeUi);
            if (fontReady) ApplyNativeWindowTheme(window, safeUi ? ss::UiTheme::Classic : app.ActiveTheme());
            const bool backendReady = fontReady && ImGui_ImplDX11_Init(gDevice.Get(), gContext.Get())
                && ImGui_ImplDX11_CreateDeviceObjects();
            if (!backendReady) {
                SetRendererLifecycle(RendererLifecycle::FatalError);
                ss::DiagnosticLog("renderer", "recovery-failed");
                MessageBoxW(window,
                            L"Grafik arayuzu kurtarilamadi. Sonalis guvenli modda yeniden baslatilmalidir.",
                            L"Sonalis arayuz hatasi", MB_OK | MB_ICONERROR);
                done = true;
                continue;
            }
            LoadLogoTexture(instance);
            app.SetLogoTexture(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(gLogoTexture.Get())));
            if (gUsingWarpRenderer) {
                app.SetStartupWarning("Grafik hizlandirma kullanilamadi; guvenli renderer etkin.");
            }
            gRendererRecoveryPending = false;
            redrawRequested = true;
            SetRendererLifecycle(gUsingWarpRenderer ? RendererLifecycle::SafeMode : RendererLifecycle::Ready);
            ss::DiagnosticLog("renderer", "recovery-completed");
        }
        if (!inputArrived && !redrawRequested && !timerElapsed) continue;
        app.ConsumeRedraw();
        const bool languageChanged = app.ConsumeFontReloadRequest();
        if (gDpiChangePending || languageChanged) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            const bool fontReady = ConfigureFontsAndStyle(
                gDpiChangePending ? gRequestedDpiScale : WindowDpiScale(window),
                app.ActiveLanguage(), app.ActiveTheme(), app.ActiveResourceProfile(),
                app.ActiveTextScale(), app.ActiveUiDensity(), app.ActiveHighContrast(),
                app.ActiveColorVisionMode(), app.ActiveCustomAccentR(),
                app.ActiveCustomAccentG(), app.ActiveCustomAccentB(), safeUi);
            if (fontReady) ApplyNativeWindowTheme(window, safeUi ? ss::UiTheme::Classic : app.ActiveTheme());
            if (!fontReady || !ImGui_ImplDX11_CreateDeviceObjects()) {
                gRendererRecoveryPending = true;
            }
            gDpiChangePending = false;
        }
        if (gFrameLatencyWaitable != nullptr) WaitForSingleObjectEx(gFrameLatencyWaitable, 1'000, TRUE);

        const auto frameStarted = std::chrono::steady_clock::now();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.Render();
        const auto uiBuilt = std::chrono::steady_clock::now();
        ss::RecordPerformance(ss::PerformanceMetric::UiFrame, static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(uiBuilt - frameStarted).count()));
        ImGui::Render();
        const ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const float clearColor[4] = {background.x, background.y, background.z, background.w};
        gContext->OMSetRenderTargets(1, gRenderTarget.GetAddressOf(), nullptr);
        gContext->ClearRenderTargetView(gRenderTarget.Get(), clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT presentResult = gSwapChain->Present(1, 0);
        if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
            ss::DiagnosticLog("renderer", "device-lost-hr="
                + std::to_string(static_cast<unsigned long>(presentResult)));
            gRendererRecoveryPending = true;
            SetRendererLifecycle(RendererLifecycle::DeviceLost);
        } else if (!firstFramePresented && SUCCEEDED(presentResult)) {
            firstFramePresented = true;
            const auto firstFrameTime = std::chrono::steady_clock::now() - startupBegan;
            if (firstFrameTime > std::chrono::seconds(2)) {
                ss::DiagnosticLog("startup-watchdog", "first-frame-over-2s");
                app.SetStartupWarning("Ilk arayuz karesi gec olusturuldu; tanilama kaydi olusturuldu.");
            }
        }
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(window, &placement)) {
        const RECT& normal = placement.rcNormalPosition;
        app.SetWindowPlacement(normal.left, normal.top, normal.right - normal.left,
                               normal.bottom - normal.top, IsZoomed(window) != FALSE);
    }
    app.Shutdown();
    namedPipeThread.request_stop();
    const HANDLE wakePipe = CreateFileW(kDeepLinkPipe, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wakePipe != INVALID_HANDLE_VALUE) CloseHandle(wakePipe);
    if (namedPipeThread.joinable()) namedPipeThread.join();
    gAppUi = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    if (SUCCEEDED(comResult)) CoUninitialize();
    CloseHandle(singleInstanceMutex);
    return 0;
}

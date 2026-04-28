#include <hyprgraphics/color/Color.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/PointerManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprlang.hpp>
#include <hyprutils/string/VarList.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

inline HANDLE         PHANDLE              = nullptr;
inline CFunctionHook* g_pOnMouseMovedHook  = nullptr;
inline bool           g_syntheticMotion    = false;
inline bool           g_unloading          = false;

constexpr auto CONFIG_NS = "plugin:hyprinertia:";

struct SConfig {
    Hyprlang::INT*    enabled      = nullptr;
    Hyprlang::FLOAT*  friction     = nullptr;
    Hyprlang::FLOAT*  multiplier   = nullptr;
    Hyprlang::FLOAT*  deadzone     = nullptr;
    Hyprlang::FLOAT*  minVelocity  = nullptr;
    Hyprlang::FLOAT*  maxVelocity  = nullptr;
    Hyprlang::INT*    tickMs       = nullptr;
    Hyprlang::INT*    idleStartMs  = nullptr;
    Hyprlang::STRING const* devices = nullptr;
};

inline SConfig g_config;

struct SDeviceState {
    Vector2D                  velocity;
    uint32_t                  lastMotionMs = 0;
    std::chrono::steady_clock::time_point lastRealMotion;
    IPointer::SMotionEvent    lastEvent;
    bool                      active = false;
};

struct SState {
    std::unordered_map<std::string, SDeviceState> devices;
    SP<CEventLoopTimer>       timer;
};

inline SState g_state;

using origOnMouseMoved = void (*)(void*, IPointer::SMotionEvent);

void inertiaTick(SP<CEventLoopTimer> self, void*);

template <typename T>
void* pmfAddress(T pmf) {
    struct PMF {
        uintptr_t ptr;
        ptrdiff_t adj;
    };

    static_assert(std::is_member_function_pointer_v<T>);
    static_assert(sizeof(T) == sizeof(PMF));

    const auto representation = std::bit_cast<PMF>(pmf);
    if (representation.ptr & 0x01)
        throw std::runtime_error("unexpected virtual function");

    return reinterpret_cast<void*>(representation.ptr);
}

CFunctionHook* hook(void* target, const std::string& signature, void* handler) {
    Dl_info info = {};
    if (!dladdr(target, &info))
        throw std::runtime_error("symbol not available");

#ifdef __GLIBCXX__
    if (signature != info.dli_sname)
        throw std::runtime_error(std::format("unexpected symbol {}", info.dli_sname));
#endif

    auto* hook = HyprlandAPI::createFunctionHook(PHANDLE, target, handler);
    if (!hook || !hook->hook())
        throw std::runtime_error("hooking failed");

    return hook;
}

Hyprlang::CConfigValue* configValue(const std::string& name) {
    return HyprlandAPI::getConfigValue(PHANDLE, std::string{CONFIG_NS} + name);
}

template <typename T>
T* configPtr(const std::string& name) {
    auto* value = configValue(name);
    return value ? *reinterpret_cast<T* const*>(value->getDataStaticPtr()) : nullptr;
}

Hyprlang::STRING const* configStringPtr(const std::string& name) {
    auto* value = configValue(name);
    return value ? reinterpret_cast<Hyprlang::STRING const*>(value->getDataStaticPtr()) : nullptr;
}

void refreshConfigPtrs() {
    g_config.enabled     = configPtr<Hyprlang::INT>("enabled");
    g_config.friction    = configPtr<Hyprlang::FLOAT>("friction");
    g_config.multiplier  = configPtr<Hyprlang::FLOAT>("multiplier");
    g_config.deadzone    = configPtr<Hyprlang::FLOAT>("deadzone");
    g_config.minVelocity = configPtr<Hyprlang::FLOAT>("min_velocity");
    g_config.maxVelocity = configPtr<Hyprlang::FLOAT>("max_velocity");
    g_config.tickMs      = configPtr<Hyprlang::INT>("tick_ms");
    g_config.idleStartMs = configPtr<Hyprlang::INT>("idle_start_ms");
    g_config.devices     = configStringPtr("devices");
}

bool enabled() {
    return g_config.enabled && *g_config.enabled;
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
        return "";
    const auto last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> splitDevices(const std::string& value) {
    std::vector<std::string> out;
    Hyprutils::String::CVarList list(value, 0, ',');
    for (const auto& item : list) {
        auto part = lower(trim(item));
        if (!part.empty())
            out.push_back(part);
    }
    return out;
}

bool deviceMatches(const SP<IPointer>& device) {
    if (!device)
        return false;

    const std::string filter = g_config.devices && *g_config.devices ? *g_config.devices : "";
    const auto        items  = splitDevices(filter);
    if (items.empty())
        return true;

    const auto hlName     = lower(device->m_hlName);
    const auto deviceName = lower(device->m_deviceName);

    for (const auto& item : items) {
        if (item == "*" || item == "all")
            return true;
        if (item == "touchpad" && device->m_isTouchpad)
            return true;
        if (item == "mouse" && !device->m_isTouchpad)
            return true;
        if (item == hlName || item == deviceName)
            return true;
    }

    return false;
}

double magnitude(const Vector2D& v) {
    return std::hypot(v.x, v.y);
}

std::string deviceKey(const SP<IPointer>& device) {
    if (!device)
        return "";
    if (!device->m_hlName.empty())
        return device->m_hlName;
    if (!device->m_deviceName.empty())
        return device->m_deviceName;
    return std::format("ptr:{:x}", reinterpret_cast<uintptr_t>(device.get()));
}

void stopTimer() {
    if (!g_state.timer)
        return;

    g_pEventLoopManager->removeTimer(g_state.timer);
    g_state.timer.reset();
}

void disarmTimer() {
    if (!g_state.timer)
        return;

    g_state.timer->updateTimeout(std::nullopt);
    g_pEventLoopManager->scheduleRecalc();
}

void disarmAll() {
    for (auto& [_, state] : g_state.devices) {
        state.velocity = {};
        state.active   = false;
    }

    disarmTimer();
}

void ensureTimer(std::chrono::milliseconds timeout) {
    if (!g_state.timer) {
        g_state.timer = makeShared<CEventLoopTimer>(timeout, inertiaTick, nullptr);
        g_pEventLoopManager->addTimer(g_state.timer);
    } else {
        g_state.timer->updateTimeout(timeout);
        g_pEventLoopManager->scheduleRecalc();
    }
}

void inertiaTick(SP<CEventLoopTimer> self, void*) {
    if (g_unloading || !enabled() || !g_pInputManager) {
        self->updateTimeout(std::nullopt);
        return;
    }

    const double minVelocity = std::max(0.0, static_cast<double>(g_config.minVelocity ? *g_config.minVelocity : 0.05F));
    const double friction    = std::clamp(static_cast<double>(g_config.friction ? *g_config.friction : 0.88F), 0.0, 0.999);
    const auto tick = std::max(1, static_cast<int>(g_config.tickMs ? *g_config.tickMs : 8));
    const auto idleStart = std::max(0, static_cast<int>(g_config.idleStartMs ? *g_config.idleStartMs : 16));
    const auto now = std::chrono::steady_clock::now();
    bool       anyActive = false;

    for (auto& [_, state] : g_state.devices) {
        if (!state.active)
            continue;

        if (!state.lastEvent.device) {
            state.active = false;
            continue;
        }

        const auto idleFor = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastRealMotion);
        if (idleFor.count() < idleStart) {
            anyActive = true;
            continue;
        }

        state.velocity = state.velocity * friction;
        if (magnitude(state.velocity) < minVelocity) {
            state.active = false;
            continue;
        }

        auto event     = state.lastEvent;
        event.delta    = state.velocity;
        event.unaccel  = state.velocity;
        event.timeMs   = state.lastMotionMs + static_cast<uint32_t>(tick);

        state.lastMotionMs = event.timeMs;

        g_syntheticMotion = true;
        reinterpret_cast<origOnMouseMoved>(g_pOnMouseMovedHook->m_original)(g_pInputManager.get(), event);
        g_syntheticMotion = false;

        anyActive = true;
    }

    if (anyActive) {
        self->updateTimeout(std::chrono::milliseconds(tick));
        g_pEventLoopManager->scheduleRecalc();
    } else {
        self->updateTimeout(std::nullopt);
    }
}

void hkOnMouseMoved(void* thisptr, IPointer::SMotionEvent event) {
    if (g_unloading || g_syntheticMotion) {
        reinterpret_cast<origOnMouseMoved>(g_pOnMouseMovedHook->m_original)(thisptr, event);
        return;
    }

    if (!enabled()) {
        disarmAll();
        reinterpret_cast<origOnMouseMoved>(g_pOnMouseMovedHook->m_original)(thisptr, event);
        return;
    }

    if (!deviceMatches(event.device)) {
        reinterpret_cast<origOnMouseMoved>(g_pOnMouseMovedHook->m_original)(thisptr, event);
        return;
    }

    const auto maxVelocity = std::max(0.0, static_cast<double>(g_config.maxVelocity ? *g_config.maxVelocity : 80.0F));
    const auto multiplier  = std::max(0.0, static_cast<double>(g_config.multiplier ? *g_config.multiplier : 1.0F));
    const auto deadzone    = std::max(0.0, static_cast<double>(g_config.deadzone ? *g_config.deadzone : 0.0F));

    auto& state          = g_state.devices[deviceKey(event.device)];
    state.velocity       = event.delta * multiplier;
    state.lastMotionMs   = event.timeMs;
    state.lastRealMotion = std::chrono::steady_clock::now();
    state.lastEvent      = event;

    const auto currentMagnitude = magnitude(state.velocity);
    if (maxVelocity > 0.0 && currentMagnitude > maxVelocity)
        state.velocity = state.velocity * (maxVelocity / currentMagnitude);

    reinterpret_cast<origOnMouseMoved>(g_pOnMouseMovedHook->m_original)(thisptr, event);

    const auto idleStart = std::max(0, static_cast<int>(g_config.idleStartMs ? *g_config.idleStartMs : 16));
    if (magnitude(state.velocity) > deadzone && magnitude(state.velocity) > static_cast<double>(g_config.minVelocity ? *g_config.minVelocity : 0.05F)) {
        state.active = true;
        ensureTimer(std::chrono::milliseconds(idleStart));
    } else {
        state.active = false;
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    g_unloading = false;

    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();
    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprinertia] failed to load, version mismatch", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error(std::format("version mismatch, built against {}, running {}", clientHash, compositorHash));
    }

    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"enabled"}, Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"devices"}, Hyprlang::STRING{"touchpad"});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"friction"}, Hyprlang::FLOAT{0.88F});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"multiplier"}, Hyprlang::FLOAT{0.70F});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"deadzone"}, Hyprlang::FLOAT{0.0F});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"min_velocity"}, Hyprlang::FLOAT{0.05F});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"max_velocity"}, Hyprlang::FLOAT{45.0F});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"tick_ms"}, Hyprlang::INT{8});
    HyprlandAPI::addConfigValue(PHANDLE, CONFIG_NS + std::string{"idle_start_ms"}, Hyprlang::INT{18});
    HyprlandAPI::reloadConfig();
    refreshConfigPtrs();

    try {
        g_pOnMouseMovedHook = hook(
            pmfAddress(&CInputManager::onMouseMoved),
            "_ZN13CInputManager12onMouseMovedEN8IPointer12SMotionEventE",
            reinterpret_cast<void*>(&hkOnMouseMoved));
    } catch (const std::exception& e) {
        HyprlandAPI::addNotification(PHANDLE, std::format("[hyprinertia] cannot load: {}", e.what()), CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw;
    }

    return {"hyprinertia", "Adds configurable pointer inertia to selected inputs", "jppan", "0.4"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_unloading = true;
    g_syntheticMotion = false;
    g_state.devices.clear();
    stopTimer();
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

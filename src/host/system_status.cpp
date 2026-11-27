// mcla-x86 host: what the machine is doing, logged at start-up. See system_status.h.
#include "system_status.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

namespace mcla_host {

namespace {

// --- Settings the queries read ---------------------------------------------------------------------------------------

// Power setting subgroup and settings (powrprof / powercfg aliases SUB_PROCESSOR, PERFBOOSTMODE, PROCTHROTTLEMAX).
constexpr const char* kSubProcessor = "54533251-82be-4824-96c1-47b60b740d00";
constexpr const char* kPerfBoostMode = "be337238-0d82-4146-a960-4f3749d470c7";
constexpr const char* kProcThrottleMax = "bc5038f7-23e0-4960-96da-33abaf5935ec";

// Windows power modes (the "overlay schemes" behind the power mode slider of Settings > System > Power): the GUIDs and names
// are in system_status_rules.h.

// PERFBOOSTMODE values, as powercfg /qh prints them.
const char* BoostModeName(uint32_t mode) {
  switch (mode) {
    case 0:
      return "Disabled";
    case 1:
      return "Enabled";
    case 2:
      return "Aggressive";
    case 3:
      return "Efficient Enabled";
    case 4:
      return "Efficient Aggressive";
    case 5:
      return "Aggressive At Guaranteed";
    case 6:
      return "Efficient Aggressive At Guaranteed";
    default:
      return "unknown mode";
  }
}

// Lenovo Intelligent Cooling (Lenovo Notebook ITS Service): the mode the vendor's own app / Fn+Q chose.
constexpr wchar_t kLenovoMmc[] = L"SYSTEM\\CurrentControlSet\\Services\\LITSSVC\\LNBITS\\IC\\MMC";

constexpr wchar_t kAsusRoot[] = L"SOFTWARE\\ASUS\\ASUS System Control Interface";
constexpr wchar_t kAsusHotkeys[] = L"SOFTWARE\\ASUS\\ASUS System Control Interface\\AsusOptimization\\ASUS Keyboard Hotkeys";
constexpr wchar_t kAsusScreenXpert[] = L"SOFTWARE\\ASUS\\ASUS System Control Interface\\AsusOptimization\\ScreenXpert";

// --- Helpers ---------------------------------------------------------------------------------------------------------

std::string Utf8FromWide(const wchar_t* wide) {
  if (!wide || !*wide) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string utf8(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
  utf8.resize(size_t(size) - 1);
  return utf8;
}

std::string GuidToString(const GUID& guid) {
  char text[40];
  std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(guid.Data1),
                unsigned(guid.Data2), unsigned(guid.Data3), unsigned(guid.Data4[0]), unsigned(guid.Data4[1]),
                unsigned(guid.Data4[2]), unsigned(guid.Data4[3]), unsigned(guid.Data4[4]), unsigned(guid.Data4[5]),
                unsigned(guid.Data4[6]), unsigned(guid.Data4[7]));
  return text;
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (no braces) to a GUID.
bool ParseGuid(std::string_view text, GUID& guid) {
  uint8_t bytes[16];
  size_t count = 0;
  int pending = -1;
  for (const char c : text) {
    if (c == '-') {
      continue;
    }
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false;
    }
    if (pending < 0) {
      pending = digit;
    } else {
      if (count >= sizeof(bytes)) {
        return false;
      }
      bytes[count++] = uint8_t(pending * 16 + digit);
      pending = -1;
    }
  }
  if (count != sizeof(bytes) || pending >= 0) {
    return false;
  }
  guid.Data1 = (unsigned long)bytes[0] << 24 | (unsigned long)bytes[1] << 16 | (unsigned long)bytes[2] << 8 | bytes[3];
  guid.Data2 = uint16_t(bytes[4] << 8 | bytes[5]);
  guid.Data3 = uint16_t(bytes[6] << 8 | bytes[7]);
  std::memcpy(guid.Data4, bytes + 8, 8);
  return true;
}

// HKLM value, REG_DWORD, 64-bit registry view, read access only.
std::optional<uint32_t> ReadHklmDword(const wchar_t* subkey, const wchar_t* name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  DWORD type = 0;
  DWORD value = 0;
  DWORD size = sizeof(value);
  const LONG result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
    return std::nullopt;
  }
  return uint32_t(value);
}

bool HklmKeyExists(const wchar_t* subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(key);
  return true;
}

// FARPROC to a typed function pointer without the function-type cast warning.
template <typename Fn>
Fn ProcAddress(HMODULE module, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// powrprof.dll, loaded at run time: the host does not link powrprof.lib, and a missing export just leaves a field empty.
struct PowerApi {
  using GetEffectiveOverlayScheme = DWORD(WINAPI*)(GUID*);
  using GetActualOverlayScheme = DWORD(WINAPI*)(GUID*);
  using GetActiveScheme = DWORD(WINAPI*)(HKEY, GUID**);
  using ReadFriendlyName = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, PUCHAR, PDWORD);
  using ReadValueIndex = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, PDWORD);

  HMODULE module = nullptr;
  GetEffectiveOverlayScheme get_effective_overlay = nullptr;
  GetActualOverlayScheme get_actual_overlay = nullptr;
  GetActiveScheme get_active_scheme = nullptr;
  ReadFriendlyName read_friendly_name = nullptr;
  ReadValueIndex read_ac_value_index = nullptr;
  ReadValueIndex read_dc_value_index = nullptr;

  PowerApi() {
    module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      return;
    }
    get_effective_overlay = ProcAddress<GetEffectiveOverlayScheme>(module, "PowerGetEffectiveOverlayScheme");
    get_actual_overlay = ProcAddress<GetActualOverlayScheme>(module, "PowerGetActualOverlayScheme");
    get_active_scheme = ProcAddress<GetActiveScheme>(module, "PowerGetActiveScheme");
    read_friendly_name = ProcAddress<ReadFriendlyName>(module, "PowerReadFriendlyName");
    read_ac_value_index = ProcAddress<ReadValueIndex>(module, "PowerReadACValueIndex");
    read_dc_value_index = ProcAddress<ReadValueIndex>(module, "PowerReadDCValueIndex");
  }
  ~PowerApi() {
    if (module) {
      FreeLibrary(module);
    }
  }
  PowerApi(const PowerApi&) = delete;
  PowerApi& operator=(const PowerApi&) = delete;
};

std::string FriendlyName(const PowerApi& api, const GUID& scheme) {
  if (!api.read_friendly_name) {
    return {};
  }
  DWORD bytes = 0;
  // The first call reports the size (ERROR_MORE_DATA), the second fills the UTF-16 name.
  api.read_friendly_name(nullptr, &scheme, nullptr, nullptr, nullptr, &bytes);
  if (bytes < sizeof(wchar_t) || bytes > 4096) {
    return {};
  }
  std::wstring name(bytes / sizeof(wchar_t) + 1, L'\0');
  if (api.read_friendly_name(nullptr, &scheme, nullptr, nullptr, reinterpret_cast<PUCHAR>(name.data()), &bytes) !=
      ERROR_SUCCESS) {
    return {};
  }
  return Utf8FromWide(name.c_str());
}

std::optional<uint32_t> ReadSchemeValue(PowerApi::ReadValueIndex read, const GUID& scheme, const GUID& subgroup,
                                        const GUID& setting) {
  if (!read) {
    return std::nullopt;
  }
  DWORD value = 0;
  if (read(nullptr, &scheme, &subgroup, &setting, &value) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return uint32_t(value);
}

void QueryPower(SystemStatus& status) {
  SYSTEM_POWER_STATUS power{};
  if (GetSystemPowerStatus(&power)) {
    status.power_source = power.ACLineStatus == 1     ? PowerSource::kAc
                          : power.ACLineStatus == 0 ? PowerSource::kBattery
                                                    : PowerSource::kUnknown;
    // BatteryFlag 128 = no system battery, 255 = unknown (then a battery is assumed and its level is unknown).
    status.has_battery = power.BatteryFlag == 255 || (power.BatteryFlag & 128) == 0;
    status.battery_percent = (status.has_battery && power.BatteryLifePercent <= 100) ? int(power.BatteryLifePercent) : -1;
    status.battery_saver = (power.SystemStatusFlag & 1) != 0;
  }

  PowerApi api;
  if (!api.module) {
    return;
  }
  GUID overlay{};
  if (api.get_effective_overlay && api.get_effective_overlay(&overlay) == ERROR_SUCCESS) {
    status.overlay_known = true;
    status.overlay_guid = GuidToString(overlay);
    const char* known = OverlayName(status.overlay_guid);
    status.overlay_name = known ? known : status.overlay_guid;  // unknown GUIDs are printed as they are
  }
  GUID actual{};
  if (api.get_actual_overlay && api.get_actual_overlay(&actual) == ERROR_SUCCESS) {
    status.overlay_actual_known = true;
    status.overlay_actual_guid = GuidToString(actual);
    const char* known = OverlayName(status.overlay_actual_guid);
    status.overlay_actual_name = known ? known : status.overlay_actual_guid;
  }

  GUID* scheme_ptr = nullptr;
  if (api.get_active_scheme && api.get_active_scheme(nullptr, &scheme_ptr) == ERROR_SUCCESS && scheme_ptr) {
    const GUID scheme = *scheme_ptr;
    LocalFree(scheme_ptr);
    status.scheme_guid = GuidToString(scheme);
    status.scheme_name = FriendlyName(api, scheme);
    status.plan_is_performance = status.scheme_guid == kPlanHighPerformance || status.scheme_guid == kPlanUltimatePerformance;
    GUID subgroup{};
    GUID boost{};
    GUID throttle_max{};
    if (ParseGuid(kSubProcessor, subgroup) && ParseGuid(kPerfBoostMode, boost) && ParseGuid(kProcThrottleMax, throttle_max)) {
      status.boost_mode_ac = ReadSchemeValue(api.read_ac_value_index, scheme, subgroup, boost);
      status.boost_mode_dc = ReadSchemeValue(api.read_dc_value_index, scheme, subgroup, boost);
      status.max_state_ac = ReadSchemeValue(api.read_ac_value_index, scheme, subgroup, throttle_max);
      status.max_state_dc = ReadSchemeValue(api.read_dc_value_index, scheme, subgroup, throttle_max);
    }
  }
}

void QueryAsus(SystemStatus& status) {
  status.asus_present = HklmKeyExists(kAsusRoot);
  if (!status.asus_present) {
    return;
  }
  status.asus_quiet_fan = ReadHklmDword(kAsusHotkeys, L"QuietFan");
  status.asus_quiet_fan_power_mode = ReadHklmDword(kAsusHotkeys, L"QuietFanPowerMode");
  status.asus_fan_mode = ReadHklmDword(kAsusScreenXpert, L"FanMode");
  if (!status.asus_fan_mode) {
    status.asus_fan_mode = ReadHklmDword(kAsusHotkeys, L"FanMode");
  }
}

void QueryLenovo(SystemStatus& status) {
  status.lenovo_present = HklmKeyExists(kLenovoMmc);
  if (!status.lenovo_present) {
    return;
  }
  status.lenovo_mode = ReadHklmDword(kLenovoMmc, L"CurrentSetting");
  status.lenovo_capability = ReadHklmDword(kLenovoMmc, L"Capability");
}

// --- NVML (nvml.dll from the NVIDIA driver, loaded at run time) -------------------------------------------------------------
//
// Questions only (nvmlDeviceGet*), never a setting. The library is loaded once and stays loaded (nvmlInit_v2 costs about
// 100 ms and the 30 s sample asks again). On a hybrid laptop the first device is the NVIDIA one; a machine without the
// driver, or one where a call is refused, leaves the field empty.
struct NvmlApi {
  using Result = int;  // nvmlReturn_t, 0 = NVML_SUCCESS
  using DeviceHandle = void*;
  HMODULE module = nullptr;
  bool ok = false;
  Result (*init)() = nullptr;
  Result (*get_count)(unsigned*) = nullptr;
  Result (*get_handle)(unsigned, DeviceHandle*) = nullptr;
  Result (*get_name)(DeviceHandle, char*, unsigned) = nullptr;
  Result (*get_enforced_limit)(DeviceHandle, unsigned*) = nullptr;
  Result (*get_default_limit)(DeviceHandle, unsigned*) = nullptr;
  Result (*get_constraints)(DeviceHandle, unsigned*, unsigned*) = nullptr;
  Result (*get_power_usage)(DeviceHandle, unsigned*) = nullptr;
  Result (*get_clock)(DeviceHandle, int, unsigned*) = nullptr;
  Result (*get_max_clock)(DeviceHandle, int, unsigned*) = nullptr;
  Result (*get_temperature)(DeviceHandle, int, unsigned*) = nullptr;
  Result (*get_reasons)(DeviceHandle, unsigned long long*) = nullptr;
  DeviceHandle device = nullptr;

  NvmlApi() {
    module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      // Older drivers keep it in C:\Program Files\NVIDIA Corporation\NVSMI. An absolute path only.
      wchar_t base[MAX_PATH] = {};
      const DWORD n = GetEnvironmentVariableW(L"ProgramW6432", base, MAX_PATH);
      if (n > 0 && n < MAX_PATH - 48) {
        const std::wstring path = std::wstring(base) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      }
    }
    if (!module) {
      return;
    }
    init = ProcAddress<Result (*)()>(module, "nvmlInit_v2");
    get_count = ProcAddress<Result (*)(unsigned*)>(module, "nvmlDeviceGetCount_v2");
    get_handle = ProcAddress<Result (*)(unsigned, DeviceHandle*)>(module, "nvmlDeviceGetHandleByIndex_v2");
    get_name = ProcAddress<Result (*)(DeviceHandle, char*, unsigned)>(module, "nvmlDeviceGetName");
    get_enforced_limit = ProcAddress<Result (*)(DeviceHandle, unsigned*)>(module, "nvmlDeviceGetEnforcedPowerLimit");
    get_default_limit = ProcAddress<Result (*)(DeviceHandle, unsigned*)>(module, "nvmlDeviceGetPowerManagementDefaultLimit");
    get_constraints =
        ProcAddress<Result (*)(DeviceHandle, unsigned*, unsigned*)>(module, "nvmlDeviceGetPowerManagementLimitConstraints");
    get_power_usage = ProcAddress<Result (*)(DeviceHandle, unsigned*)>(module, "nvmlDeviceGetPowerUsage");
    get_clock = ProcAddress<Result (*)(DeviceHandle, int, unsigned*)>(module, "nvmlDeviceGetClockInfo");
    get_max_clock = ProcAddress<Result (*)(DeviceHandle, int, unsigned*)>(module, "nvmlDeviceGetMaxClockInfo");
    get_temperature = ProcAddress<Result (*)(DeviceHandle, int, unsigned*)>(module, "nvmlDeviceGetTemperature");
    get_reasons = ProcAddress<Result (*)(DeviceHandle, unsigned long long*)>(module, "nvmlDeviceGetCurrentClocksThrottleReasons");
    if (!get_reasons) {
      get_reasons = ProcAddress<Result (*)(DeviceHandle, unsigned long long*)>(module, "nvmlDeviceGetCurrentClocksEventReasons");
    }
    unsigned count = 0;
    if (!init || !get_count || !get_handle || init() != 0 || get_count(&count) != 0 || count == 0 ||
        get_handle(0, &device) != 0) {
      device = nullptr;
      return;
    }
    ok = true;
  }
  // The library stays loaded for the life of the process (the watchdog thread may still ask during shutdown).
};

NvmlApi& Nvml() {
  static NvmlApi api;  // thread-safe initialisation
  return api;
}

void QueryGpu(SystemStatus& status) {
  NvmlApi& nv = Nvml();
  if (!nv.ok) {
    return;
  }
  char name[96] = {};
  unsigned value = 0, low = 0, high = 0;
  if (nv.get_name && nv.get_name(nv.device, name, sizeof(name)) == 0) {
    status.gpu_name = name;
  }
  if (nv.get_enforced_limit && nv.get_enforced_limit(nv.device, &value) == 0) status.gpu_limit_mw = value;
  if (nv.get_default_limit && nv.get_default_limit(nv.device, &value) == 0) status.gpu_default_mw = value;
  if (nv.get_constraints && nv.get_constraints(nv.device, &low, &high) == 0) {
    status.gpu_min_mw = low;
    status.gpu_max_mw = high;
  }
  if (nv.get_power_usage && nv.get_power_usage(nv.device, &value) == 0) status.gpu_draw_mw = value;
  if (nv.get_clock && nv.get_clock(nv.device, 0 /*NVML_CLOCK_GRAPHICS*/, &value) == 0) status.gpu_clock_mhz = value;
  if (nv.get_max_clock && nv.get_max_clock(nv.device, 0, &value) == 0) status.gpu_clock_max_mhz = value;
  if (nv.get_temperature && nv.get_temperature(nv.device, 0 /*NVML_TEMPERATURE_GPU*/, &value) == 0) status.gpu_temp_c = int(value);
  unsigned long long reasons = 0;
  if (nv.get_reasons && nv.get_reasons(nv.device, &reasons) == 0) {
    status.gpu_reasons_known = true;
    status.gpu_reasons = reasons;
  }
  status.gpu_known = status.gpu_limit_mw > 0 || status.gpu_draw_mw > 0 || !status.gpu_name.empty();
}

// NVML's clocks throttle reasons, as short words (nvml.h nvmlClocksThrottleReason*).
std::string ThrottleReasonsText(uint64_t reasons) {
  struct Bit {
    uint64_t bit;
    const char* name;
  };
  static constexpr Bit kBits[] = {{0x1, "idle"},          {0x2, "application clocks"}, {0x4, "software power cap"},
                                  {0x8, "hardware slowdown"}, {0x10, "sync boost"},      {0x20, "software thermal"},
                                  {0x40, "hardware thermal"}, {0x80, "power brake"},     {0x100, "display clock"}};
  std::string text;
  for (const Bit& b : kBits) {
    if (reasons & b.bit) {
      text += (text.empty() ? "" : "+") + std::string(b.name);
    }
  }
  return text.empty() ? std::string("none") : text;
}

void QueryDisplay(SystemStatus& status, HWND hwnd) {
  const HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) {
    return;
  }
  status.display_device = Utf8FromWide(info.szDevice);
  status.display_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    return;
  }
  status.display_known = true;
  status.display_width = int(mode.dmPelsWidth);
  status.display_height = int(mode.dmPelsHeight);
  // 0 and 1 mean "the hardware's default" in dmDisplayFrequency, not a rate.
  status.display_hz = (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1 ? int(mode.dmDisplayFrequency) : 0;
  status.display_dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
}

void QueryCpu(SystemStatus& status) {
  status.cpu_logical = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  ULONG length = 0;
  GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
  if (length == 0 || length > (1u << 20)) {
    return;
  }
  std::vector<uint8_t> buffer(length);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length,
                                  GetCurrentProcess(), 0)) {
    return;
  }
  std::map<int, int, std::greater<int>> per_class;
  int total = 0;
  for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
    const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (entry->Size == 0 || offset + entry->Size > length) {
      break;
    }
    if (entry->Type == CpuSetInformation) {
      ++per_class[entry->CpuSet.EfficiencyClass];
      ++total;
    }
    offset += entry->Size;
  }
  if (total > 0) {
    status.cpu_logical = total;
    status.cpu_classes.assign(per_class.begin(), per_class.end());
  }
}

// --- Text ------------------------------------------------------------------------------------------------------------

std::string Number(uint32_t value) { return std::to_string(value); }

std::string Optional(const std::optional<uint32_t>& value) { return value ? Number(*value) : std::string("n/a"); }

std::string PowerSourceText(const SystemStatus& status) {
  std::string text;
  switch (status.power_source) {
    case PowerSource::kAc:
      text = "AC";
      break;
    case PowerSource::kBattery:
      text = "BATTERY";
      break;
    default:
      text = "unknown";
      break;
  }
  if (!status.has_battery) {
    text += " (no battery)";
  } else {
    text += status.battery_percent >= 0 ? ", battery " + std::to_string(status.battery_percent) + " %" : ", battery unknown";
    text += status.battery_saver ? ", battery saver ON" : ", battery saver off";
  }
  return text;
}

std::string CpuClassesText(const SystemStatus& status) {
  std::string text = "cpu: " + std::to_string(status.cpu_logical) + " logical";
  if (!status.cpu_classes.empty()) {
    text += " (";
    bool first = true;
    for (const auto& [efficiency_class, count] : status.cpu_classes) {
      if (!first) {
        text += ", ";
      }
      text += std::to_string(count) + (first ? " at efficiency class " : " at class ") + std::to_string(efficiency_class);
      first = false;
    }
    text += ")";
  }
  return text;
}

std::string BoostText(const SystemStatus& status) {
  auto boost = [](const std::optional<uint32_t>& mode) {
    return mode ? std::string(BoostModeName(*mode)) + " (" + Number(*mode) + ")" : std::string("n/a");
  };
  auto percent = [](const std::optional<uint32_t>& value) { return value ? Number(*value) + " %" : std::string("n/a"); };
  return "plan boost mode AC " + boost(status.boost_mode_ac) + ", DC " + boost(status.boost_mode_dc) +
         "; max processor state AC " + percent(status.max_state_ac) + ", DC " + percent(status.max_state_dc);
}

std::string PowerModeText(const SystemStatus& status) {
  std::string mode = status.overlay_known ? status.overlay_name : std::string("unknown");
  // The slider and the plan: with the High performance plan active the slider is off, and the effective mode reads as
  // Balanced although the slider (Settings > System > Power) may say Best performance.
  if (status.overlay_known && status.overlay_actual_known && status.overlay_actual_guid != status.overlay_guid) {
    mode += " in effect (the slider says " + status.overlay_actual_name + ", but " +
            (status.scheme_name.empty() ? std::string("the active plan") : "the " + status.scheme_name + " plan") +
            " switches it off)";
  }
  return mode;
}

std::string GpuText(const SystemStatus& status) {
  if (!status.gpu_known) {
    return "gpu: no NVIDIA driver library (nvml.dll) or no device: power limit unknown";
  }
  std::string text = "gpu: " + (status.gpu_name.empty() ? std::string("NVIDIA") : status.gpu_name) + ", power limit ";
  text += status.gpu_limit_mw > 0 ? Watts(status.gpu_limit_mw) + " W enforced" : std::string("unknown");
  if (status.gpu_default_mw > 0 || status.gpu_max_mw > 0) {
    text += " (board default " + (status.gpu_default_mw > 0 ? Watts(status.gpu_default_mw) : std::string("?")) + " W, range " +
            (status.gpu_min_mw > 0 ? Watts(status.gpu_min_mw) : std::string("?")) + "-" +
            (status.gpu_max_mw > 0 ? Watts(status.gpu_max_mw) : std::string("?")) + " W)";
  }
  if (GpuPowerCapped(status)) {
    text += " CAPPED to " + std::to_string(uint64_t(status.gpu_limit_mw) * 100 / status.gpu_max_mw) + " % of the maximum";
  }
  if (status.gpu_draw_mw > 0) {
    text += "; drawing " + Watts(status.gpu_draw_mw) + " W now";
  }
  if (status.gpu_clock_mhz > 0) {
    text += ", graphics clock " + std::to_string(status.gpu_clock_mhz) + (status.gpu_clock_max_mhz > 0 ? " of " + std::to_string(status.gpu_clock_max_mhz) : std::string()) + " MHz";
  }
  if (status.gpu_temp_c >= 0) {
    text += ", " + std::to_string(status.gpu_temp_c) + " C";
  }
  if (status.gpu_reasons_known) {
    text += ", throttle reasons: " + ThrottleReasonsText(status.gpu_reasons);
  }
  return text;
}

std::string LenovoText(const SystemStatus& status) {
  std::string text = "lenovo: Intelligent Cooling mode ";
  text += status.lenovo_mode ? std::string(LenovoModeName(*status.lenovo_mode)) + " (registry CurrentSetting " +
                                   std::to_string(*status.lenovo_mode) + ")"
                             : std::string("unreadable");
  if (status.lenovo_capability) {
    text += ", capability mask " + std::to_string(*status.lenovo_capability);
  }
  return text;
}

std::string DisplayText(const SystemStatus& status) {
  if (!status.display_known) {
    return "display: unknown";
  }
  std::string text = "display: " + std::to_string(status.display_width) + "x" + std::to_string(status.display_height) + " @ " +
                     (status.display_hz > 0 ? std::to_string(status.display_hz) + " Hz" : std::string("? Hz"));
  if (status.display_dpi > 0) {
    text += ", " + std::to_string(status.display_dpi) + " dpi (" + std::to_string((status.display_dpi * 100 + 48) / 96) + " %)";
  }
  if (!status.display_device.empty()) {
    text += " on " + status.display_device + (status.display_primary ? " (primary)" : "");
  }
  return text;
}

}  // namespace

SystemStatus QuerySystemStatus(void* native_window) {
  SystemStatus status;
  HWND hwnd = static_cast<HWND>(native_window);
  if (hwnd && !IsWindow(hwnd)) {
    hwnd = nullptr;
  }
  QueryPower(status);
  QueryAsus(status);
  QueryLenovo(status);
  QueryGpu(status);
  QueryDisplay(status, hwnd);
  QueryCpu(status);
  return status;
}

std::string SystemStatusOneLine(const SystemStatus& status) {
  std::string line;
  auto add = [&](const std::string& part) {
    if (!line.empty()) {
      line += " | ";
    }
    line += part;
  };
  switch (status.power_source) {
    case PowerSource::kAc:
      add("AC power");
      break;
    case PowerSource::kBattery:
      add(status.battery_percent >= 0 ? "On battery " + std::to_string(status.battery_percent) + " %" : "On battery");
      break;
    default:
      add("power source unknown");
      break;
  }
  if (status.battery_saver) {
    add("battery saver ON");
  }
  if (status.overlay_known) {
    add("mode " + status.overlay_name);
  }
  if (status.lenovo_present && status.lenovo_mode) {
    add(std::string("Lenovo ") + LenovoModeName(*status.lenovo_mode));
  }
  if (status.gpu_known && status.gpu_limit_mw > 0) {
    add("GPU limit " + Watts(status.gpu_limit_mw) + (status.gpu_max_mw > 0 ? " of " + Watts(status.gpu_max_mw) : std::string()) + " W");
  }
  if (status.display_known) {
    add((status.display_hz > 0 ? std::to_string(status.display_hz) + " Hz " : std::string()) +
        std::to_string(status.display_width) + "x" + std::to_string(status.display_height));
  }
  add(std::to_string(status.cpu_logical) + " logical cpus");
  return line;
}

void LogSystemStatus(const SystemStatus& status, int target_fps, int guest_height) {
  // 1: power source, Windows power mode and plan.
  const std::string mode = PowerModeText(status);
  std::string plan = status.scheme_guid.empty() ? std::string("unknown")
                                                : (status.scheme_name.empty() ? status.scheme_guid : status.scheme_name);
  REXLOG_INFO("[mcla-sys] power: {}; Windows power mode: {}; plan: {}", PowerSourceText(status), mode, plan);

  // 2: cpu layout and the plan's boost settings.
  REXLOG_INFO("[mcla-sys] {}; {}", CpuClassesText(status), BoostText(status));

  // 3: the vendor's performance mode: ASUS fan values (raw), Lenovo Intelligent Cooling.
  if (status.asus_present) {
    REXLOG_INFO("[mcla-sys] asus fan profile raw QuietFan={} FanMode={} QuietFanPowerMode={} (mapping unverified)",
                Optional(status.asus_quiet_fan), Optional(status.asus_fan_mode), Optional(status.asus_quiet_fan_power_mode));
  }
  if (status.lenovo_present) {
    REXLOG_INFO("[mcla-sys] {}", LenovoText(status));
  }
  if (!status.asus_present && !status.lenovo_present) {
    REXLOG_INFO("[mcla-sys] vendor mode: no ASUS System Control Interface or Lenovo Intelligent Cooling key found");
  }
  // 3b: the GPU's power limit against its maximum (NVML).
  REXLOG_INFO("[mcla-sys] {}", GpuText(status));

  // 4: display and presentation mode. The swap chain is inside the SDK, so whether the present got independent flip is
  // not knowable from here.
  const bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  REXLOG_INFO("[mcla-sys] {}; fullscreen={}; PresentMon's PresentMode column says whether it got independent flip",
              DisplayText(status), fullscreen ? "true" : "false");

  // 5: the warning.
  const std::string warning = SystemStatusWarning(status, target_fps, guest_height);
  if (!warning.empty()) {
    REXLOG_WARN("[mcla-sys] WARNING: {}", warning);
  }
}


// ---- the periodic sample (watchdog thread, every 30 s) --------------------------------------------------------------

namespace {

// CallNtPowerInformation(ProcessorInformation) fills one of these per logical processor (documented in the
// CallNtPowerInformation reference; not in the SDK headers).
struct ProcessorPowerInformation {
  ULONG Number;
  ULONG MaxMhz;
  ULONG CurrentMhz;
  ULONG MhzLimit;
  ULONG MaxIdleState;
  ULONG CurrentIdleState;
};

struct SampleState {
  bool init = false;
  std::vector<int> cls;  // efficiency class per logical processor index (-1 unknown)
  ULONGLONG cpu_100ns = 0;
  ULONGLONG wall_100ns = 0;
  DWORD page_faults = 0;
  ULONGLONG read_bytes = 0;
  using CallNtPowerInformationFn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
  CallNtPowerInformationFn call_power = nullptr;
};

ULONGLONG FileTime100ns(const FILETIME& t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

}  // namespace

void LogSystemSample() {
  static SampleState st;
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const DWORD count = si.dwNumberOfProcessors;
  if (!st.init) {
    st.init = true;
    st.cls.assign(count, -1);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length) {
      std::vector<uint8_t> buffer(length);
      if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length,
                                     GetCurrentProcess(), 0)) {
        for (size_t offset = 0; offset + 8 <= length;) {
          const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
          if (entry->Size == 0) break;
          if (entry->Type == CpuSetInformation && entry->CpuSet.Group == 0 &&
              entry->CpuSet.LogicalProcessorIndex < count) {
            st.cls[entry->CpuSet.LogicalProcessorIndex] = entry->CpuSet.EfficiencyClass;
          }
          offset += entry->Size;
        }
      }
    }
    if (HMODULE module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      st.call_power = ProcAddress<SampleState::CallNtPowerInformationFn>(module, "CallNtPowerInformation");
    }
  }

  // Power source.
  SYSTEM_POWER_STATUS power{};
  std::string source = "power ?";
  if (GetSystemPowerStatus(&power)) {
    source = power.ACLineStatus == 1 ? "AC" : power.ACLineStatus == 0 ? "BATTERY" : "power ?";
    if (power.BatteryLifePercent <= 100) source += fmt::format(" {} %", power.BatteryLifePercent);
    if (power.SystemStatusFlag & 1) source += ", battery saver ON";
  }

  // Clocks per efficiency class: average current MHz and the lowest limit against the maximum. A limit under the
  // maximum is the firmware or Windows capping the clock (thermal, power, a quiet mode).
  std::string clocks;
  if (st.call_power && count > 0) {
    std::vector<ProcessorPowerInformation> info(count);
    constexpr int kProcessorInformation = 11;
    if (st.call_power(kProcessorInformation, nullptr, 0, info.data(),
                      ULONG(sizeof(ProcessorPowerInformation) * count)) == 0) {
      std::map<int, std::vector<const ProcessorPowerInformation*>, std::greater<int>> by_class;
      for (DWORD i = 0; i < count; ++i) by_class[i < st.cls.size() ? st.cls[i] : -1].push_back(&info[i]);
      for (const auto& [cls, list] : by_class) {
        uint64_t sum = 0;
        ULONG max_mhz = 0, min_limit = ~0u;
        for (const ProcessorPowerInformation* p : list) {
          sum += p->CurrentMhz;
          max_mhz = std::max(max_mhz, p->MaxMhz);
          min_limit = std::min(min_limit, p->MhzLimit);
        }
        clocks += fmt::format("{}class{} {} MHz (max {}, limit {}{})", clocks.empty() ? "" : ", ", cls,
                              sum / std::max<size_t>(1, list.size()), max_mhz, min_limit,
                              min_limit < max_mhz ? " CAPPED" : "");
      }
    }
  }

  // The process: cores' worth of CPU used since the last sample, memory, paging and reads.
  FILETIME create{}, exit{}, kernel{}, user{}, now_ft{};
  GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
  GetSystemTimeAsFileTime(&now_ft);
  const ULONGLONG cpu = FileTime100ns(kernel) + FileTime100ns(user);
  const ULONGLONG wall = FileTime100ns(now_ft);
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  IO_COUNTERS io{};
  GetProcessIoCounters(GetCurrentProcess(), &io);
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  GlobalMemoryStatusEx(&mem);
  std::string usage = "process: first sample";
  if (st.wall_100ns && wall > st.wall_100ns) {
    const double seconds = double(wall - st.wall_100ns) / 1e7;
    usage = fmt::format("process: {:.2f} cores, {} page faults/s, reads {:.1f} MiB/s",
                        double(cpu - st.cpu_100ns) / 1e7 / seconds,
                        uint64_t(double(pmc.PageFaultCount - st.page_faults) / seconds),
                        double(io.ReadTransferCount - st.read_bytes) / seconds / (1024.0 * 1024.0));
  }
  st.cpu_100ns = cpu;
  st.wall_100ns = wall;
  st.page_faults = pmc.PageFaultCount;
  st.read_bytes = io.ReadTransferCount;
  // The GPU: draw against the enforced limit, clock, temperature, what holds it back (NVML questions, a few microseconds).
  SystemStatus gpu_status;
  QueryGpu(gpu_status);
  std::string gpu = "gpu ?";
  if (gpu_status.gpu_known) {
    gpu = "gpu " + Watts(gpu_status.gpu_draw_mw) + " W of " + (gpu_status.gpu_limit_mw > 0 ? Watts(gpu_status.gpu_limit_mw) : std::string("?")) +
          " W, " + std::to_string(gpu_status.gpu_clock_mhz) + " MHz, " + std::to_string(gpu_status.gpu_temp_c) + " C, throttle " +
          (gpu_status.gpu_reasons_known ? ThrottleReasonsText(gpu_status.gpu_reasons) : std::string("?"));
  }
  REXLOG_INFO("[mcla-sys] sample: {} | cpu {} | {} | {}, working set {} MiB, private {} MiB | system memory {} % used",
              source, clocks.empty() ? std::string("clocks ?") : clocks, gpu, usage, uint64_t(pmc.WorkingSetSize >> 20),
              uint64_t(pmc.PrivateUsage >> 20), uint32_t(mem.dwMemoryLoad));
}

}  // namespace mcla_host

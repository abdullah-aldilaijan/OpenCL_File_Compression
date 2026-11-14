// Port of hardware.py: CPU model names plus usable OpenCL GPUs for the device dropdown.
#include <algorithm>
#include <cctype>
#include <set>

#include "common.h"
#include "ocl.h"

#include <cfgmgr32.h>
#include <setupapi.h>

static std::vector<std::string> cpu_names() {
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (int index = 0; index < 1024; index++) {
        std::wstring key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\" + std::to_wstring(index);
        wchar_t value[256];
        DWORD size = sizeof value;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, value,
                         &size) != ERROR_SUCCESS)
            break;
        std::string name = utf8(value);
        size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
        if (a == std::string::npos) continue;
        name = name.substr(a, b - a + 1);
        if (seen.insert(name).second) names.push_back(name);
    }
    return names;
}

namespace {

// A display adapter as Windows Plug and Play sees it (independent of OpenCL).
struct DisplayAdapter {
    std::string name;
    bool intel = false;
    bool working = false;  // started without a problem code
    ULONG problem = 0;
};

std::vector<DisplayAdapter> display_adapters() {
    static const GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    std::vector<DisplayAdapter> found;
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVINFO_DATA info{sizeof(SP_DEVINFO_DATA)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++) {
        DisplayAdapter a;
        wchar_t text[512];
        if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(text),
                                              sizeof text, nullptr) ||
            SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(text),
                                              sizeof text, nullptr))
            a.name = utf8(text);
        if (SetupDiGetDeviceInstanceIdW(set, &info, text, 512, nullptr)) {
            std::wstring id = fold_case(text);
            a.intel = id.find(L"ven_8086") != std::wstring::npos;
        }
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS) {
            a.working = (status & DN_STARTED) && !(status & DN_HAS_PROBLEM);
            a.problem = (status & DN_HAS_PROBLEM) ? problem : 0;
        }
        found.push_back(a);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// Intel Core desktop/laptop CPUs ship with integrated graphics unless the model ends in F (no iGPU).
bool intel_cpu_with_igpu(const std::string& cpu) {
    if (cpu.find("Intel") == std::string::npos) return false;
    size_t at = cpu.find(" i3-");
    if (at == std::string::npos) at = cpu.find(" i5-");
    if (at == std::string::npos) at = cpu.find(" i7-");
    if (at == std::string::npos) at = cpu.find(" i9-");
    if (at == std::string::npos) return false;
    size_t end = cpu.find(' ', at + 1);
    std::string model = cpu.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    return !model.empty() && std::toupper(static_cast<unsigned char>(model.back())) != 'F';
}

std::string has_intel(const std::vector<ocl::Device>& devices) {
    for (auto& d : devices)
        if (d.name.find("Intel") != std::string::npos || d.platform_name.find("Intel") != std::string::npos)
            return d.name;
    return {};
}

// Explains GPUs that are physically there but that OpenCL cannot use.
void explain_missing_gpus(const std::vector<std::string>& cpus, const std::vector<ocl::Device>& all_gpus,
                          std::vector<std::string>& notices) {
    std::vector<DisplayAdapter> adapters = display_adapters();
    bool intel_adapter = std::any_of(adapters.begin(), adapters.end(), [](auto& a) { return a.intel; });
    bool igpu_cpu = std::any_of(cpus.begin(), cpus.end(), intel_cpu_with_igpu);
    std::string intel_ocl = has_intel(all_gpus);

    if (igpu_cpu && !intel_adapter) {
        notices.push_back(
            "This CPU normally has Intel integrated graphics, but Windows does not see an Intel display adapter, "
            "so OpenCL cannot use it. The firmware has most likely switched it off because a discrete GPU is "
            "installed. To use it: enable the integrated graphics in the BIOS/UEFI (often called 'Integrated "
            "Graphics', 'iGPU Multi-Monitor' or 'IGD', set to Enabled while the primary display stays on the "
            "discrete GPU), then install Intel's graphics driver, which includes the OpenCL runtime. Some "
            "prebuilt PCs do not offer this setting.");
    }
    for (auto& a : adapters) {
        if (!a.intel || !intel_ocl.empty()) continue;
        if (a.problem == CM_PROB_DISABLED)
            notices.push_back(a.name + " is disabled in Device Manager. Enable it there to use it with OpenCL.");
        else if (!a.working || a.name.find("Basic Display") != std::string::npos)
            notices.push_back(a.name + " has no working driver (problem code " + std::to_string(a.problem) +
                              "). Install Intel's graphics driver, which includes the OpenCL runtime.");
        else
            notices.push_back(a.name + " is working, but no Intel OpenCL runtime was found. Install or repair "
                                       "Intel's graphics driver (it registers the OpenCL runtime).");
    }
    for (auto& d : all_gpus) {
        if (!d.available) notices.push_back(d.name + " is reported by OpenCL as not available.");
        else if (!d.compiler) notices.push_back(d.name + " has no OpenCL compiler and cannot be used.");
    }
}

}  // namespace

std::vector<DeviceOption> discover_devices(std::vector<std::string>& notices) {
    std::vector<std::string> cpus = cpu_names();
    if (cpus.empty()) {
        notices.push_back("CPU model lookup unavailable; using system CPU information.");
        cpus.push_back("System processor");
    }
    std::vector<ocl::Device> gpus, all_gpus;
    try {
        all_gpus = ocl::devices(ocl::CL_DEVICE_TYPE_GPU, &notices);
        for (auto& d : all_gpus)
            if (d.available && d.compiler) gpus.push_back(d);
        std::stable_sort(gpus.begin(), gpus.end(), [](const ocl::Device& a, const ocl::Device& b) {
            if (a.unified != b.unified) return !a.unified;  // discrete GPUs first
            return a.global_mem > b.global_mem;
        });
    } catch (const std::exception&) {
        notices.push_back("No usable OpenCL GPU runtime found. CPU mode remains available.");
    }
    explain_missing_gpus(cpus, all_gpus, notices);
    std::vector<DeviceOption> options;
    auto has_label = [&](const std::string& label) {
        return std::any_of(options.begin(), options.end(), [&](const DeviceOption& o) { return o.label == label; });
    };
    for (auto& name : cpus) options.push_back({"CPU \xE2\x80\x94 " + name, "CPU"});
    for (auto& d : gpus) {
        std::string label = "GPU \xE2\x80\x94 " + d.name;
        if (has_label(label)) label += " (" + d.platform_name + ")";
        options.push_back({label, d.name});
    }
    // Extra option: the CPU and every GPU compress the same archive together.
    if (!gpus.empty())
        options.push_back({std::string("All \xE2\x80\x94 CPU + ") + (gpus.size() == 1 ? "GPU" : "all GPUs") +
                               " together",
                           "ALL"});
    if (gpus.empty() && notices.empty())
        notices.push_back("No OpenCL GPU with compiler support was found. CPU mode remains available.");
    return options;
}

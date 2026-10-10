#include "GraphicsDiag.hpp"
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>

namespace launcher {

void GraphicsDiag::Refresh() {
    gpus.clear();
    vulkanAvailable = false;
    loaderError.clear();
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { loaderError = "vulkan-1.dll not found: install GPU driver / Vulkan runtime"; return; }
    auto pCreate = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(vk, "vkCreateInstance"));
    auto pEnum = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(vk, "vkEnumeratePhysicalDevices"));
    auto pProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetProcAddress(vk, "vkGetPhysicalDeviceProperties"));
    auto pDestroy = reinterpret_cast<PFN_vkDestroyInstance>(GetProcAddress(vk, "vkDestroyInstance"));
    if (!pCreate || !pEnum || !pProps) { loaderError = "Vulkan loader exports missing"; FreeLibrary(vk); return; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (pCreate(&ci, nullptr, &inst) != VK_SUCCESS) { loaderError = "vkCreateInstance failed"; FreeLibrary(vk); return; }
    uint32_t n = 0;
    if (pEnum(inst, &n, nullptr) != VK_SUCCESS || n == 0) {
        loaderError = "no Vulkan physical devices";
        if (pDestroy) pDestroy(inst, nullptr);
        FreeLibrary(vk);
        return;
    }
    std::vector<VkPhysicalDevice> devs(n);
    pEnum(inst, &n, devs.data());
    for (auto d : devs) {
        VkPhysicalDeviceProperties p{};
        pProps(d, &p);
        GpuInfo g;
        g.name = p.deviceName;
        switch (p.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: g.type = "discrete"; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: g.type = "integrated"; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: g.type = "virtual"; break;
            default: g.type = "other"; break;
        }
        char api[32];
        snprintf(api, sizeof(api), "%u.%u.%u", VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
        g.api = api;
        gpus.push_back(g);
    }
    vulkanAvailable = true;
    if (pDestroy) pDestroy(inst, nullptr);
    FreeLibrary(vk);
}

const char* GraphicsDiag::PresentModeText() { return "VK_PRESENT_MODE_FIFO_KHR (hardcoded in engine)"; }
const char* GraphicsDiag::VsyncText() { return "ON — flips wait for vblank (~60 Hz); no off switch in engine"; }

} // namespace launcher

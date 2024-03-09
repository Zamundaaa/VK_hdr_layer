#define VK_USE_PLATFORM_WAYLAND_KHR
#include "vkroots.h"
#include "frog-color-management-v1-client-protocol.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <ranges>

using namespace std::literals;

namespace HdrLayer
{

static bool contains(const std::vector<const char *> vec, std::string_view lookupValue)
{
    return std::ranges::any_of(vec, [&lookupValue](const auto &value) {
        return value == lookupValue;
    });
}

struct ColorDescription {
    VkSurfaceFormat2KHR surface;
    frog_color_managed_surface_primaries primaries;
    frog_color_managed_surface_transfer_function transferFunction;
    bool extended_volume;
};

static std::vector<ColorDescription> s_ExtraHDRSurfaceFormats = {
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                VK_COLOR_SPACE_HDR10_ST2084_EXT,
            }
        },
        .primaries = FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC2020,
        .transferFunction = FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ,
        .extended_volume = false,
    },
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                VK_COLOR_SPACE_HDR10_ST2084_EXT,
            }
        },
        .primaries = FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC2020,
        .transferFunction = FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ,
        .extended_volume = false,
    },
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
            }
        },
        .primaries = FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709,
        .transferFunction = FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_SCRGB_LINEAR,
        .extended_volume = true,
    },
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_COLOR_SPACE_BT709_LINEAR_EXT,
            }
        },
        .primaries = FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709,
        .transferFunction = FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_SCRGB_LINEAR,
        .extended_volume = true,
    },
    // ColorDescription{
    //     .surface = {
    //         .surfaceFormat = {
    //             VK_FORMAT_R16G16B16A16_SFLOAT,
    //             VK_COLOR_SPACE_BT2020_LINEAR_EXT,
    //         }
    //     },
    //     .primaries_cicp = 9,
    //     .tf_cicp = 8,
    //     .extended_volume = false,
    // },
    // ColorDescription{
    //     .surface = {
    //         .surfaceFormat = {
    //             VK_FORMAT_R16G16B16A16_SFLOAT,
    //             VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT,
    //         }
    //     },
    //     .primaries_cicp = 13,
    //     .tf_cicp = 8,
    //     .extended_volume = false,
    // },
    // ColorDescription{
    //     .surface = {
    //         .surfaceFormat = {
    //             VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    //             VK_COLOR_SPACE_BT709_NONLINEAR_EXT,
    //         }
    //     },
    //     .primaries_cicp = 6,
    //     .tf_cicp = 6,
    //     .extended_volume = true,
    // },
    // ColorDescription{
    //     .surface = {
    //         .surfaceFormat = {
    //             VK_FORMAT_A2R10G10B10_UNORM_PACK32,
    //             VK_COLOR_SPACE_BT709_NONLINEAR_EXT,
    //         }
    //     },
    //     .primaries_cicp = 6,
    //     .tf_cicp = 6,
    //     .extended_volume = true,
    // },
};

struct HdrSurfaceData {
    VkInstance instance;

    wl_display *display;
    wl_event_queue *queue;
    frog_color_management_factory_v1 *colorManagement;

    wl_surface *surface;
    frog_color_managed_surface *colorSurface;
};
VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSurface, VkSurfaceKHR);

struct HdrSwapchainData {
    VkSurfaceKHR surface;
    int primaries;
    int tf;

    VkHdrMetadataEXT metadata;
    bool desc_dirty;
};
VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSwapchain, VkSwapchainKHR);

enum DescStatus {
    WAITING,
    READY,
    FAILED,
};

class VkInstanceOverrides
{
public:
    static VkResult CreateInstance(
        PFN_vkCreateInstance pfnCreateInstanceProc,
        const VkInstanceCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkInstance *pInstance)
    {
        auto enabledExts = std::vector<const char *>(
                               pCreateInfo->ppEnabledExtensionNames,
                               pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);

        if (contains(enabledExts, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
            std::erase(enabledExts, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }

        VkInstanceCreateInfo createInfo = *pCreateInfo;
        createInfo.enabledExtensionCount = uint32_t(enabledExts.size());
        createInfo.ppEnabledExtensionNames = enabledExts.data();

        return pfnCreateInstanceProc(&createInfo, pAllocator, pInstance);
    }

    static VkResult CreateWaylandSurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
    {
        auto queue = wl_display_create_queue(pCreateInfo->display);
        wl_registry *registry = wl_display_get_registry(pCreateInfo->display);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);

        VkResult res = pDispatch->CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
        if (res != VK_SUCCESS) {
            return res;
        }

        {
            auto hdrSurface = HdrSurface::create(*pSurface, HdrSurfaceData{
                .instance = instance,
                .display = pCreateInfo->display,
                .queue = queue,
                .colorManagement = nullptr,
                .surface = pCreateInfo->surface,
                .colorSurface = nullptr,
            });

            wl_registry_add_listener(registry, &s_registryListener, reinterpret_cast<void *>(hdrSurface.get()));
            wl_display_dispatch_queue(pCreateInfo->display, queue);
            wl_display_roundtrip_queue(pCreateInfo->display, queue); // get globals
            wl_display_roundtrip_queue(pCreateInfo->display, queue); // get features/supported_cicps/etc
            wl_registry_destroy(registry);
        }

        if (!HdrSurface::get(*pSurface)->colorManagement) {
            fprintf(stderr, "[HDR Layer] wayland compositor lacking frog color management protocol..\n");

            HdrSurface::remove(*pSurface);
            return VK_SUCCESS;
        }

        auto hdrSurface = HdrSurface::get(*pSurface);

        frog_color_managed_surface *colorSurface = frog_color_management_factory_v1_get_color_managed_surface(hdrSurface->colorManagement, pCreateInfo->surface);
        frog_color_managed_surface_add_listener(colorSurface, &color_surface_interface_listener, nullptr);
        wl_display_flush(hdrSurface->display);

        hdrSurface->colorSurface = colorSurface;

        fprintf(stderr, "[HDR Layer] Created HDR surface\n");
        return VK_SUCCESS;
    }

    static VkResult GetPhysicalDeviceSurfaceFormatsKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t *pSurfaceFormatCount,
        VkSurfaceFormatKHR *pSurfaceFormats)
    {
        auto hdrSurface = HdrSurface::get(surface);
        if (!hdrSurface)
            return pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);

        uint32_t count = 0;
        std::vector<VkFormat> pixelFormats = {};
        auto result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
        if (result != VK_SUCCESS) {
            return result;
        }
        std::vector<VkSurfaceFormatKHR> formats(count);
        result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());
        if (result != VK_SUCCESS) {
            return result;
        }

        for (uint32_t i = 0; i < count; i++) {
            pixelFormats.push_back(formats[i].format);
        }

        std::vector<VkSurfaceFormatKHR> extraFormats = {};
        for (auto desc = s_ExtraHDRSurfaceFormats.begin(); desc != s_ExtraHDRSurfaceFormats.end(); ++desc) {
            fprintf(stderr, "[HDR Layer] Enabling format: %u colorspace: %u\n", desc->surface.surfaceFormat.format, desc->surface.surfaceFormat.colorSpace);
            extraFormats.push_back(desc->surface.surfaceFormat);
        }

        return vkroots::helpers::append(
                   pDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
                   extraFormats,
                   pSurfaceFormatCount,
                   pSurfaceFormats,
                   physicalDevice,
                   surface);
    }

    static VkResult GetPhysicalDeviceSurfaceFormats2KHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
        uint32_t *pSurfaceFormatCount,
        VkSurfaceFormat2KHR *pSurfaceFormats)
    {
        auto hdrSurface = HdrSurface::get(pSurfaceInfo->surface);
        if (!hdrSurface) {
            return pDispatch->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
        }

        uint32_t count = 0;
        std::vector<VkFormat> pixelFormats = {};
        auto result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, &count, nullptr);
        if (result != VK_SUCCESS) {
            return result;
        }
        std::vector<VkSurfaceFormatKHR> formats(count);
        result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, &count, formats.data());
        if (result != VK_SUCCESS) {
            return result;
        }
        for (uint32_t i = 0; i < count; i++) {
            pixelFormats.push_back(formats[i].format);
        }

        std::vector<VkSurfaceFormat2KHR> extraFormats = {};
        for (auto desc = s_ExtraHDRSurfaceFormats.begin(); desc != s_ExtraHDRSurfaceFormats.end(); ++desc) {
            fprintf(stderr, "[HDR Layer] Enabling format: %u colorspace: %u\n", desc->surface.surfaceFormat.format, desc->surface.surfaceFormat.colorSpace);
            extraFormats.push_back(desc->surface);
        }

        return vkroots::helpers::append(
                   pDispatch->GetPhysicalDeviceSurfaceFormats2KHR,
                   extraFormats,
                   pSurfaceFormatCount,
                   pSurfaceFormats,
                   physicalDevice,
                   pSurfaceInfo);
    }

    static void DestroySurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks *pAllocator)
    {
        if (auto state = HdrSurface::get(surface)) {
            frog_color_managed_surface_destroy(state->colorSurface);
            frog_color_management_factory_v1_destroy(state->colorManagement);
            wl_event_queue_destroy(state->queue);
        }
        HdrSurface::remove(surface);
        pDispatch->DestroySurfaceKHR(instance, surface, pAllocator);
    }

    static VkResult
    EnumerateDeviceExtensionProperties(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        const char *pLayerName,
        uint32_t *pPropertyCount,
        VkExtensionProperties *pProperties)
    {
        static constexpr std::array<VkExtensionProperties, 1> s_LayerExposedExts = {{
                {
                    VK_EXT_HDR_METADATA_EXTENSION_NAME,
                    VK_EXT_HDR_METADATA_SPEC_VERSION
                },
            }
        };

        if (pLayerName) {
            if (pLayerName == "VK_LAYER_hdr_wsi"sv) {
                return vkroots::helpers::array(s_LayerExposedExts, pPropertyCount, pProperties);
            } else {
                return pDispatch->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
            }
        }

        return vkroots::helpers::append(
                   pDispatch->EnumerateDeviceExtensionProperties,
                   s_LayerExposedExts,
                   pPropertyCount,
                   pProperties,
                   physicalDevice,
                   pLayerName);
    }

private:
    static constexpr struct frog_color_managed_surface_listener color_surface_interface_listener {
      .preferred_metadata = [](void *data,
                               struct frog_color_managed_surface *frog_color_managed_surface,
                               uint32_t transfer_function,
                               uint32_t output_display_primary_red_x,
                               uint32_t output_display_primary_red_y,
                               uint32_t output_display_primary_green_x,
                               uint32_t output_display_primary_green_y,
                               uint32_t output_display_primary_blue_x,
                               uint32_t output_display_primary_blue_y,
                               uint32_t output_white_point_x,
                               uint32_t output_white_point_y,
                               uint32_t max_luminance,
                               uint32_t min_luminance,
                               uint32_t max_full_frame_luminance){}
    };

    static constexpr wl_registry_listener s_registryListener = {
        .global = [](void *data, wl_registry * registry, uint32_t name, const char *interface, uint32_t version)
        {
            auto surface = reinterpret_cast<HdrSurfaceData *>(data);

            if (interface == "frog_color_management_factory_v1"sv) {
                surface->colorManagement = reinterpret_cast<frog_color_management_factory_v1 *>(
                    wl_registry_bind(registry, name, &frog_color_management_factory_v1_interface, version));
            }
        },
        .global_remove = [](void *data, wl_registry * registry, uint32_t name) {},
    };
};

class VkDeviceOverrides
{
public:
    static void DestroySwapchainKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks *pAllocator)
    {
        HdrSwapchain::remove(swapchain);
        pDispatch->DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    static VkResult CreateSwapchainKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        const VkSwapchainCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSwapchainKHR *pSwapchain)
    {
        auto hdrSurface = HdrSurface::get(pCreateInfo->surface);
        if (!hdrSurface)
            return pDispatch->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

        VkSwapchainCreateInfoKHR swapchainInfo = *pCreateInfo;

        if (hdrSurface) {
            // If this is a custom surface
            // Force the colorspace to sRGB before sending to the driver.
            swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

            fprintf(stderr, "[HDR Layer] Creating swapchain for id: %u - format: %s - colorspace: %s\n",
                    wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(hdrSurface->surface)),
                    vkroots::helpers::enumString(pCreateInfo->imageFormat),
                    vkroots::helpers::enumString(pCreateInfo->imageColorSpace));
        }

        // Check for VkFormat support and return VK_ERROR_INITIALIZATION_FAILED
        // if that VkFormat is unsupported for the underlying surface.
        {
            std::vector<VkSurfaceFormatKHR> supportedSurfaceFormats;
            vkroots::helpers::enumerate(
                pDispatch->pPhysicalDeviceDispatch->pInstanceDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
                supportedSurfaceFormats,
                pDispatch->PhysicalDevice,
                swapchainInfo.surface);

            bool supportedSwapchainFormat = std::find_if(
                                                supportedSurfaceFormats.begin(),
                                                supportedSurfaceFormats.end(),
            [ = ](VkSurfaceFormatKHR value) {
                return value.format == swapchainInfo.imageFormat;
            }) != supportedSurfaceFormats.end();

            if (!supportedSwapchainFormat) {
                fprintf(stderr, "[HDR Layer] Refusing to make swapchain (unsupported VkFormat) for id: %u - format: %s - colorspace: %s\n",
                        wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(hdrSurface->surface)),
                        vkroots::helpers::enumString(pCreateInfo->imageFormat),
                        vkroots::helpers::enumString(pCreateInfo->imageColorSpace));

                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        VkResult result = pDispatch->CreateSwapchainKHR(device, &swapchainInfo, pAllocator, pSwapchain);
        if (hdrSurface && result == VK_SUCCESS) {
            // alpha mode is ignored
            frog_color_managed_surface_primaries primaries = FROG_COLOR_MANAGED_SURFACE_PRIMARIES_UNDEFINED;
            frog_color_managed_surface_transfer_function tf = FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_UNDEFINED;
            for (auto desc = s_ExtraHDRSurfaceFormats.begin(); desc != s_ExtraHDRSurfaceFormats.end(); ++desc) {
                if (desc->surface.surfaceFormat.colorSpace == pCreateInfo->imageColorSpace) {
                    primaries = desc->primaries;
                    tf = desc->transferFunction;
                    break;
                }
            }

            if (primaries == 0 && tf == 0 && pCreateInfo->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                fprintf(stderr, "[HDR Layer] Unknown color space, assuming untagged");
            };

            HdrSwapchain::create(*pSwapchain, HdrSwapchainData{
                .surface = pCreateInfo->surface,
                .primaries = primaries,
                .tf = tf,
                .desc_dirty = true,
            });
        }
        return result;
    }

    static void
    SetHdrMetadataEXT(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        uint32_t swapchainCount,
        const VkSwapchainKHR *pSwapchains,
        const VkHdrMetadataEXT *pMetadata)
    {
        for (uint32_t i = 0; i < swapchainCount; i++) {
            auto hdrSwapchain = HdrSwapchain::get(pSwapchains[i]);
            if (!hdrSwapchain) {
                fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Swapchain %u does not support HDR.\n", i);
                continue;
            }

            auto hdrSurface = HdrSurface::get(hdrSwapchain->surface);
            if (!hdrSurface) {
                fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
                abort();
            }

            const VkHdrMetadataEXT &metadata = pMetadata[i];

            fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: mastering luminance min %f nits, max %f nits\n", metadata.minLuminance, metadata.maxLuminance);
            fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: maxContentLightLevel %f nits\n", metadata.maxContentLightLevel);
            fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: maxFrameAverageLightLevel %f nits\n", metadata.maxFrameAverageLightLevel);

            hdrSwapchain->metadata = metadata;
            hdrSwapchain->desc_dirty = true;
        }
    }

    static VkResult QueuePresentKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkQueue queue,
        const VkPresentInfoKHR *pPresentInfo)
    {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            if (auto hdrSwapchain = HdrSwapchain::get(pPresentInfo->pSwapchains[i])) {
                if (hdrSwapchain->desc_dirty) {
                    auto hdrSurface = HdrSurface::get(hdrSwapchain->surface);
                    const auto &metadata = hdrSwapchain->metadata;
                    frog_color_managed_surface_set_known_container_color_volume(hdrSurface->colorSurface, hdrSwapchain->primaries);
                    frog_color_managed_surface_set_known_transfer_function(hdrSurface->colorSurface, hdrSwapchain->tf);
                    frog_color_managed_surface_set_hdr_metadata(hdrSurface->colorSurface,
                                                                uint32_t(round(metadata.displayPrimaryRed.x * 10000.0)),
                                                                uint32_t(round(metadata.displayPrimaryRed.y * 10000.0)),
                                                                uint32_t(round(metadata.displayPrimaryGreen.x * 10000.0)),
                                                                uint32_t(round(metadata.displayPrimaryGreen.y * 10000.0)),
                                                                uint32_t(round(metadata.displayPrimaryBlue.x * 10000.0)),
                                                                uint32_t(round(metadata.displayPrimaryBlue.y * 10000.0)),
                                                                uint32_t(round(metadata.whitePoint.x * 10000.0)),
                                                                uint32_t(round(metadata.whitePoint.y * 10000.0)),
                                                                uint32_t(round(metadata.maxLuminance)),
                                                                uint32_t(round(metadata.minLuminance * 10000.0)),
                                                                uint32_t(round(metadata.maxContentLightLevel)),
                                                                uint32_t(round(metadata.maxFrameAverageLightLevel)));
                    hdrSwapchain->desc_dirty = false;
                }
            }
        }

        return pDispatch->QueuePresentKHR(queue, pPresentInfo);
    }
};
}

VKROOTS_DEFINE_LAYER_INTERFACES(HdrLayer::VkInstanceOverrides,
                                vkroots::NoOverrides,
                                HdrLayer::VkDeviceOverrides);

VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrSurface);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrSwapchain);

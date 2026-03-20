#define VK_USE_PLATFORM_WAYLAND_KHR
#include "vkroots.h"
#include "color-management-v1-client-protocol.h"

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

struct ColorDescription {
    VkSurfaceFormat2KHR surface;
    wp_color_manager_v1_primaries primaries;
    wp_color_manager_v1_transfer_function transferFunction;
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
        .primaries = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
        .transferFunction = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
        .extended_volume = false,
    },
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                VK_COLOR_SPACE_HDR10_ST2084_EXT,
            }
        },
        .primaries = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
        .transferFunction = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
        .extended_volume = false,
    },
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
            }
        },
        .primaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
        .transferFunction = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
        .extended_volume = true,
    },
    ColorDescription{
        .surface = {
            .surfaceFormat = {
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_COLOR_SPACE_BT709_LINEAR_EXT,
            }
        },
        .primaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
        .transferFunction = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
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
    bool supportsPassthrough = false;

    wl_display *display;
    wl_event_queue *queue;
    wp_color_manager_v1 *colorManager;

    std::vector<wp_color_manager_v1_feature> supportedFeatures;
    std::vector<wp_color_manager_v1_primaries> supportedPrimaries;
    std::vector<wp_color_manager_v1_transfer_function> supportedTransferFunctions;

    wl_surface *surface;
    wp_color_management_surface_v1 *colorSurface;
};
VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSurface, VkSurfaceKHR);

struct HdrSwapchainData {
    VkSurfaceKHR surface;

    wp_color_manager_v1_primaries primaries;
    wp_color_manager_v1_transfer_function transferFunction;
    bool untagged = false;

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

        auto hdrSurface = HdrSurface::create(*pSurface, HdrSurfaceData{
            .instance = instance,
            .supportsPassthrough = false,
            .display = pCreateInfo->display,
            .queue = queue,
            .colorManager = nullptr,
            .surface = pCreateInfo->surface,
            .colorSurface = nullptr,
        });

        wl_registry_add_listener(registry, &s_registryListener, reinterpret_cast<void *>(hdrSurface.get()));
        wl_display_dispatch_queue(pCreateInfo->display, queue);
        wl_display_roundtrip_queue(pCreateInfo->display, queue); // get globals
        wl_display_roundtrip_queue(pCreateInfo->display, queue); // get features/supported_cicps/etc
        wl_registry_destroy(registry);

        if (!hdrSurface->colorManager) {
            fprintf(stderr, "[HDR Layer] wayland compositor is lacking support for the color management protocol..\n");

            HdrSurface::remove(*pSurface);
            return VK_SUCCESS;
        }

        const bool hasParametric = std::ranges::find(hdrSurface->supportedFeatures, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) != hdrSurface->supportedFeatures.end();
        if (!hasParametric) {
            fprintf(stderr, "[HDR Layer] wayland compositor is lacking support for parametric image descriptions\n");
            HdrSurface::remove(*pSurface);
            return VK_SUCCESS;
        }
        hdrSurface->colorSurface = wp_color_manager_v1_get_surface(hdrSurface->colorManager, pCreateInfo->surface);

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
        auto result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
        if (result != VK_SUCCESS) {
            return result;
        }
        std::vector<VkSurfaceFormatKHR> formats(count);
        result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());
        if (result != VK_SUCCESS) {
            return result;
        }

        hdrSurface->supportsPassthrough = std::ranges::any_of(formats, [](const VkSurfaceFormatKHR fmt) {
            return fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT;
        });

        std::vector<VkSurfaceFormatKHR> extraFormats = {};
        for (const auto &desc : s_ExtraHDRSurfaceFormats) {
            const bool alreadySupportsColorspace = std::ranges::any_of(formats, [&desc](const VkSurfaceFormatKHR fmt) {
                return desc.surface.surfaceFormat.format == fmt.format
                    && desc.surface.surfaceFormat.colorSpace == fmt.colorSpace;
            });
            if (alreadySupportsColorspace) {
                continue;
            }
            bool hasFormat = std::ranges::any_of(formats, [&desc](const VkSurfaceFormatKHR fmt) {
                return desc.surface.surfaceFormat.format == fmt.format;
            });
            if (hdrSurface->colorSurface) {
                hasFormat &= std::ranges::find(hdrSurface->supportedPrimaries, desc.primaries) != hdrSurface->supportedPrimaries.end();
                hasFormat &= std::ranges::find(hdrSurface->supportedTransferFunctions, desc.transferFunction) != hdrSurface->supportedTransferFunctions.end();
                hasFormat &= !desc.extended_volume || std::ranges::find(hdrSurface->supportedFeatures, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME) != hdrSurface->supportedFeatures.end();
            }
            if (hasFormat) {
                fprintf(stderr, "[HDR Layer] Enabling format: %u colorspace: %u\n", desc.surface.surfaceFormat.format, desc.surface.surfaceFormat.colorSpace);
                extraFormats.push_back(desc.surface.surfaceFormat);
            }
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
        auto result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, &count, nullptr);
        if (result != VK_SUCCESS) {
            return result;
        }
        std::vector<VkSurfaceFormatKHR> formats(count);
        result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, pSurfaceInfo->surface, &count, formats.data());
        if (result != VK_SUCCESS) {
            return result;
        }

        hdrSurface->supportsPassthrough = std::ranges::any_of(formats, [](const VkSurfaceFormatKHR fmt) {
            return fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT;
        });

        std::vector<VkSurfaceFormat2KHR> extraFormats = {};
        for (const auto &desc : s_ExtraHDRSurfaceFormats) {
            const bool alreadySupportsColorspace = std::ranges::any_of(formats, [&desc](const VkSurfaceFormatKHR fmt) {
                return desc.surface.surfaceFormat.format == fmt.format
                    && desc.surface.surfaceFormat.colorSpace == fmt.colorSpace;
            });
            if (alreadySupportsColorspace) {
                continue;
            }
            bool hasFormat = std::ranges::any_of(formats, [&desc](const VkSurfaceFormatKHR fmt) {
                return desc.surface.surfaceFormat.format == fmt.format;
            });
            if (hdrSurface->colorSurface) {
                hasFormat &= std::ranges::find(hdrSurface->supportedPrimaries, desc.primaries) != hdrSurface->supportedPrimaries.end();
                hasFormat &= std::ranges::find(hdrSurface->supportedTransferFunctions, desc.transferFunction) != hdrSurface->supportedTransferFunctions.end();
            }
            if (hasFormat) {
                fprintf(stderr, "[HDR Layer] Enabling format: %u colorspace: %u\n", desc.surface.surfaceFormat.format, desc.surface.surfaceFormat.colorSpace);
                extraFormats.push_back(desc.surface);
            }
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
            if (state->colorSurface) {
                wp_color_management_surface_v1_destroy(state->colorSurface);
            }
            if (state->colorManager) {
                wp_color_manager_v1_destroy(state->colorManager);
            }
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
    static constexpr wp_color_manager_v1_listener s_colorManagerListener {
        .supported_intent = [](void *data, wp_color_manager_v1 *wp_color_manager_v4, uint32_t render_intent) {
        },
        .supported_feature = [](void *data, wp_color_manager_v1 *wp_color_manager_v4, uint32_t feature) {
            reinterpret_cast<HdrSurfaceData *>(data)->supportedFeatures.push_back(wp_color_manager_v1_feature(feature));
        },
        .supported_tf_named = [](void *data, wp_color_manager_v1 *wp_color_manager_v4, uint32_t tf) {
            reinterpret_cast<HdrSurfaceData *>(data)->supportedTransferFunctions.push_back(wp_color_manager_v1_transfer_function(tf));
        },
        .supported_primaries_named = [](void *data, wp_color_manager_v1 *wp_color_manager_v4, uint32_t primaries) {
            reinterpret_cast<HdrSurfaceData *>(data)->supportedPrimaries.push_back(wp_color_manager_v1_primaries(primaries));
        },
        .done = [](void *data, wp_color_manager_v1 *wp_color_manager_v4) {
        },
    };

    static constexpr wl_registry_listener s_registryListener = {
        .global = [](void *data, wl_registry * registry, uint32_t name, const char *interface, uint32_t version)
        {
            auto surface = reinterpret_cast<HdrSurfaceData *>(data);

            if (interface == "wp_color_manager_v1"sv) {
                surface->colorManager = reinterpret_cast<wp_color_manager_v1 *>(wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1));
                wp_color_manager_v1_add_listener(surface->colorManager, &s_colorManagerListener, surface);
            }
        },
        .global_remove = [](void *data, wl_registry * registry, uint32_t name) {},
    };
};

static constexpr wp_image_description_v1_listener s_imageDescriptionListener {
    .failed = [](void *userData, wp_image_description_v1 *descr, uint32_t cause, const char *reason) {
        fprintf(stderr, "[HDR Layer] creating image description failed! %s", reason);
        *reinterpret_cast<bool *>(userData) = true;
    },
    .ready = [](void *userData, wp_image_description_v1 *descr, uint32_t id) {
        *reinterpret_cast<bool *>(userData) = true;
    },
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
            // If this is a custom surface, force the colorspace to something the driver won't touch
            if (hdrSurface->supportsPassthrough) {
                swapchainInfo.imageColorSpace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
            } else {
                swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            }

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

            bool supportedSwapchainFormat = std::ranges::find_if(supportedSurfaceFormats, [=](VkSurfaceFormatKHR value) {
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
        if (hdrSurface && hdrSurface->colorSurface && result == VK_SUCCESS) {
            const auto it = std::ranges::find_if(s_ExtraHDRSurfaceFormats, [&pCreateInfo](const ColorDescription &description) {
                return description.surface.surfaceFormat.colorSpace == pCreateInfo->imageColorSpace;
            });
            if (it != s_ExtraHDRSurfaceFormats.end()) {
                const auto &description = *it;
                HdrSwapchain::create(*pSwapchain, HdrSwapchainData{
                    .surface = pCreateInfo->surface,
                    .primaries = description.primaries,
                    .transferFunction = description.transferFunction,
                    .desc_dirty = true,
                });
            } else {
                if (pCreateInfo->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    fprintf(stderr, "[HDR Layer] Unknown colorspace %d, assuming untagged\n", pCreateInfo->imageColorSpace);
                }
                HdrSwapchain::create(*pSwapchain, HdrSwapchainData{
                    .surface = pCreateInfo->surface,
                    .untagged = true,
                    .desc_dirty = true,
                });
            }
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
                    if (hdrSurface->colorSurface) {
                        if (hdrSwapchain->untagged) {
                            wp_color_management_surface_v1_unset_image_description(hdrSurface->colorSurface);
                        } else {
                            constexpr double primaryUnit = 1'000'000.0;
                            const auto creator = wp_color_manager_v1_create_parametric_creator(hdrSurface->colorManager);
                            wp_image_description_creator_params_v1_set_primaries_named(creator, hdrSwapchain->primaries);
                            wp_image_description_creator_params_v1_set_tf_named(creator, hdrSwapchain->transferFunction);
                            wp_image_description_creator_params_v1_set_max_fall(creator, std::round(metadata.maxFrameAverageLightLevel));
                            wp_image_description_creator_params_v1_set_max_cll(creator, std::round(metadata.maxContentLightLevel));
                            const bool hasMasteringPrimaries = std::ranges::find(hdrSurface->supportedFeatures, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) != hdrSurface->supportedFeatures.end();
                            if (hasMasteringPrimaries) {
                                if (metadata.maxLuminance > metadata.minLuminance) wp_image_description_creator_params_v1_set_mastering_luminance(creator, std::round(metadata.minLuminance * 10'000.0), std::round(metadata.maxLuminance));
                                wp_image_description_creator_params_v1_set_mastering_display_primaries(creator,
                                                                                                       std::round(metadata.displayPrimaryRed.x * primaryUnit),
                                                                                                       std::round(metadata.displayPrimaryRed.y * primaryUnit),
                                                                                                       std::round(metadata.displayPrimaryGreen.x * primaryUnit),
                                                                                                       std::round(metadata.displayPrimaryGreen.y * primaryUnit),
                                                                                                       std::round(metadata.displayPrimaryBlue.x * primaryUnit),
                                                                                                       std::round(metadata.displayPrimaryBlue.y * primaryUnit),
                                                                                                       std::round(metadata.whitePoint.x * primaryUnit),
                                                                                                       std::round(metadata.whitePoint.y * primaryUnit));
                            }
                            const bool hasCustomLuminance = std::ranges::find(hdrSurface->supportedFeatures, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) != hdrSurface->supportedFeatures.end();
                            if (hasCustomLuminance && hdrSwapchain->transferFunction == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR) {
                                // NOTE that this assumes that this is Windows-style scRGB
                                wp_image_description_creator_params_v1_set_luminances(creator, 0, 80, 203);
                            }
                            const auto imageDescription = wp_image_description_creator_params_v1_create(creator);

                            bool done = false;
                            wp_image_description_v1_add_listener(imageDescription, &s_imageDescriptionListener, &done);
                            wl_display_dispatch_queue(hdrSurface->display, hdrSurface->queue);
                            // In theory the compositor could wait for a while here. In practice it doesn't.
                            while (!done) {
                                wl_display_roundtrip_queue(hdrSurface->display, hdrSurface->queue);
                            }
                            wp_color_management_surface_v1_set_image_description(hdrSurface->colorSurface, imageDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
                            wp_image_description_v1_destroy(imageDescription);
                        }
                    }
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

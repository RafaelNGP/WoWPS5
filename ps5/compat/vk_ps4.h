#ifndef VK_PS4_H
#define VK_PS4_H
/*
 * vk_ps4.h, PS5 edition. The PS4 port's Vulkan ICD (GNM) exposed a few
 * diagnostic bridges beyond the Khronos API. RADV on the PS5 has none of them,
 * so the declarations stay (the shared console code compiles) and the inline
 * bodies report "not available". Every caller already treats a failure as
 * "skip the diagnostic".
 */
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkPs4ScanoutInfo {
    void *pixels;
    VkDeviceSize byteSize;
    uint32_t width;
    uint32_t height;
    uint32_t rowPitchBytes;
    uint32_t bytesPerPixel;
    VkFormat vkFormat;
    uint32_t nativeVideoOutFormat;
    uint32_t imageIndex;
    VkBool32 acquired;
} VkPs4ScanoutInfo;

static inline VkResult vkPs4GetAcquiredScanoutInfo(VkSwapchainKHR swapchain, uint32_t imageIndex,
                                                   VkPs4ScanoutInfo *pInfo) {
    (void)swapchain; (void)imageIndex; (void)pInfo;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static inline VkResult vkPs4GetSwapchainImageMemory(VkSwapchainKHR swapchain, uint32_t imageIndex,
                                                    VkDeviceMemory *pMemory) {
    (void)swapchain; (void)imageIndex;
    if (pMemory) *pMemory = VK_NULL_HANDLE;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

/* RADV images created OPTIMAL are tiled, swapchain images included. */
static inline VkBool32 vkPs4ImageHasLinearStorage(VkImage image) {
    (void)image;
    return VK_FALSE;
}

typedef enum VkPs4DepthInspectionReject {
    VK_PS4_DEPTH_REJECT_ARGUMENT = 1u << 0,
    VK_PS4_DEPTH_REJECT_DEVICE = 1u << 1,
} VkPs4DepthInspectionReject;

typedef struct VkPs4DepthInspection {
    uint32_t sampleCount;
    uint32_t finiteCount;
    uint32_t nonClearCount;
    uint32_t invalidCount;
    float minDepth;
    float maxDepth;
    uint32_t rejectionReason;
    uint32_t memoryTypeIndex;
} VkPs4DepthInspection;

static inline VkResult vk_ps4_InspectRetiredDepthImage(VkDevice device, VkImage image,
                                                       const VkRect2D *region,
                                                       VkPs4DepthInspection *result) {
    (void)device; (void)image; (void)region;
    if (result) {
        const VkPs4DepthInspection none = {0, 0, 0, 0, 0.0f, 0.0f, 0, 0};
        *result = none;
        result->rejectionReason = VK_PS4_DEPTH_REJECT_DEVICE;
        result->memoryTypeIndex = UINT32_MAX;
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

#ifdef __cplusplus
}
#endif

#endif /* VK_PS4_H */

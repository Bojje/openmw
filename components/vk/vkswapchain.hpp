#ifndef OPENMW_COMPONENTS_VK_VKSWAPCHAIN_H
#define OPENMW_COMPONENTS_VK_VKSWAPCHAIN_H

#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Device;

    class Swapchain
    {
    public:
        Swapchain(Device& device, VkSurfaceKHR surface, uint32_t width, uint32_t height);
        ~Swapchain();

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;
        Swapchain(Swapchain&& other) noexcept;
        Swapchain& operator=(Swapchain&& other) noexcept;

        void recreate(uint32_t width, uint32_t height);

        VkExtent2D extent() const { return mExtent; }
        VkFormat format() const { return mFormat; }
        VkImage image(uint32_t imageIndex) const { return mImages.at(imageIndex); }
        const std::vector<VkImageView>& imageViews() const { return mImageViews; }
        uint32_t imageCount() const { return static_cast<uint32_t>(mImages.size()); }
        VkSwapchainKHR handle() const { return mSwapchain; }

    private:
        struct SurfaceDetails
        {
            VkSurfaceCapabilitiesKHR capabilities;
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };

        void create(uint32_t width, uint32_t height);
        void cleanup();
        void createImageViews();
        SurfaceDetails querySurfaceDetails() const;
        VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
        VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
        VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) const;
        VkFormat findDepthFormat() const;

        Device& mDevice;
        VkSurfaceKHR mSurface;
        VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
        VkFormat mFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D mExtent = {};
        std::vector<VkImage> mImages;
        std::vector<VkImageView> mImageViews;
    };
}

#endif

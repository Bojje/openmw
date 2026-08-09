#include "vkswapchain.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "vkcommon.hpp"
#include "vkdevice.hpp"

namespace Vk
{
    Swapchain::Swapchain(Device& device, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool readback)
        : mDevice(device)
        , mSurface(surface)
        , mReadback(readback)
    {
        create(width, height);
    }

    Swapchain::~Swapchain()
    {
        cleanup();
    }

    void Swapchain::recreate(uint32_t width, uint32_t height)
    {
        cleanup();
        create(width, height);
    }

    void Swapchain::create(uint32_t width, uint32_t height)
    {
        auto details = querySurfaceDetails();
        auto surfaceFormat = chooseSurfaceFormat(details.formats);
        auto presentMode = choosePresentMode(details.presentModes);
        auto extent = chooseExtent(details.capabilities, width, height);

        // The surface minimum is sufficient for the renderer's explicit
        // per-image presentation semaphore ownership. Avoid allocating an
        // unnecessary extra image, which matters for headless and
        // memory-constrained implementations.
        uint32_t imageCount = details.capabilities.minImageCount;
        if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount)
            imageCount = details.capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = mSurface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        if (mReadback && (details.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
            throw std::runtime_error("Vulkan swapchain does not support image readback");
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (mReadback)
            createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        auto indices = mDevice.indices();
        uint32_t queueFamilyIndices[] = { indices.graphics.value(), indices.present.value() };

        if (indices.graphics != indices.present)
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = details.capabilities.currentTransform;
        createInfo.compositeAlpha = chooseCompositeAlpha(details.capabilities.supportedCompositeAlpha);
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        VK_CHECK(vkCreateSwapchainKHR(mDevice.handle(), &createInfo, nullptr, &mSwapchain));

        mFormat = surfaceFormat.format;
        mExtent = extent;

        uint32_t swapImageCount = 0;
        vkGetSwapchainImagesKHR(mDevice.handle(), mSwapchain, &swapImageCount, nullptr);
        mImages.resize(swapImageCount);
        vkGetSwapchainImagesKHR(mDevice.handle(), mSwapchain, &swapImageCount, mImages.data());

        createImageViews();
    }

    void Swapchain::cleanup()
    {
        VkDevice device = mDevice.handle();

        for (auto view : mImageViews)
            vkDestroyImageView(device, view, nullptr);
        mImageViews.clear();
        mImages.clear();

        if (mSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
        }
    }

    void Swapchain::createImageViews()
    {
        mImageViews.resize(mImages.size());

        for (size_t i = 0; i < mImages.size(); ++i)
        {
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = mImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = mFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            VK_CHECK(vkCreateImageView(mDevice.handle(), &viewInfo, nullptr, &mImageViews[i]));
        }
    }

    Swapchain::SurfaceDetails Swapchain::querySurfaceDetails() const
    {
        SurfaceDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice.physical(), mSurface, &details.capabilities);

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice.physical(), mSurface, &formatCount, nullptr);
        if (formatCount > 0)
        {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(mDevice.physical(), mSurface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(mDevice.physical(), mSurface, &presentModeCount, nullptr);
        if (presentModeCount > 0)
        {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                mDevice.physical(), mSurface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const
    {
        if (available.empty())
            throw std::runtime_error("No surface formats available");

        for (const auto& format : available)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB
                && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
        }
        return available[0];
    }

    VkPresentModeKHR Swapchain::choosePresentMode(const std::vector<VkPresentModeKHR>& available) const
    {
        for (const auto& mode : available)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                return mode;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkCompositeAlphaFlagBitsKHR Swapchain::chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) const
    {
        constexpr VkCompositeAlphaFlagBitsKHR preferred[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };

        for (const auto mode : preferred)
        {
            if ((supported & mode) != 0)
                return mode;
        }
        throw std::runtime_error("Vulkan surface has no supported composite alpha mode");
    }

    VkExtent2D Swapchain::chooseExtent(
        const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) const
    {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
            return capabilities.currentExtent;

        VkExtent2D extent = { width, height };
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height
            = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return extent;
    }

}

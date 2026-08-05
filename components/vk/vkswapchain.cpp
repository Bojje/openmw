#include "vkswapchain.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "vkcommon.hpp"
#include "vkdevice.hpp"

namespace Vk
{
    Swapchain::Swapchain(Device& device, VkSurfaceKHR surface, uint32_t width, uint32_t height)
        : mDevice(device)
        , mSurface(surface)
    {
        create(width, height);
    }

    Swapchain::~Swapchain()
    {
        cleanup();
    }

    Swapchain::Swapchain(Swapchain&& other) noexcept
        : mDevice(other.mDevice)
        , mSurface(other.mSurface)
        , mSwapchain(other.mSwapchain)
        , mFormat(other.mFormat)
        , mExtent(other.mExtent)
        , mImages(std::move(other.mImages))
        , mImageViews(std::move(other.mImageViews))
        , mDepthImage(other.mDepthImage)
        , mDepthImageMemory(other.mDepthImageMemory)
        , mDepthImageView(other.mDepthImageView)
    {
        other.mSwapchain = VK_NULL_HANDLE;
        other.mDepthImage = VK_NULL_HANDLE;
        other.mDepthImageMemory = VK_NULL_HANDLE;
        other.mDepthImageView = VK_NULL_HANDLE;
    }

    Swapchain& Swapchain::operator=(Swapchain&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();

            mSurface = other.mSurface;
            mSwapchain = other.mSwapchain;
            mFormat = other.mFormat;
            mExtent = other.mExtent;
            mImages = std::move(other.mImages);
            mImageViews = std::move(other.mImageViews);
            mDepthImage = other.mDepthImage;
            mDepthImageMemory = other.mDepthImageMemory;
            mDepthImageView = other.mDepthImageView;

            other.mSwapchain = VK_NULL_HANDLE;
            other.mDepthImage = VK_NULL_HANDLE;
            other.mDepthImageMemory = VK_NULL_HANDLE;
            other.mDepthImageView = VK_NULL_HANDLE;
        }
        return *this;
    }

    void Swapchain::recreate(uint32_t width, uint32_t height)
    {
        vkDeviceWaitIdle(mDevice.handle());
        cleanup();
        create(width, height);
    }

    void Swapchain::create(uint32_t width, uint32_t height)
    {
        auto details = querySurfaceDetails();
        auto surfaceFormat = chooseSurfaceFormat(details.formats);
        auto presentMode = choosePresentMode(details.presentModes);
        auto extent = chooseExtent(details.capabilities, width, height);

        uint32_t imageCount = details.capabilities.minImageCount + 1;
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
        if ((details.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
            throw std::runtime_error("Vulkan swapchain does not support image readback");
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

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
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
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
        createDepthResources();
    }

    void Swapchain::cleanup()
    {
        VkDevice device = mDevice.handle();

        if (mDepthImageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, mDepthImageView, nullptr);
            mDepthImageView = VK_NULL_HANDLE;
        }
        if (mDepthImage != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, mDepthImage, nullptr);
            mDepthImage = VK_NULL_HANDLE;
        }
        if (mDepthImageMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, mDepthImageMemory, nullptr);
            mDepthImageMemory = VK_NULL_HANDLE;
        }
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

    void Swapchain::createDepthResources()
    {
        VkFormat depthFormat = findDepthFormat();

        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = depthFormat;
        imageInfo.extent.width = mExtent.width;
        imageInfo.extent.height = mExtent.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VK_CHECK(vkCreateImage(mDevice.handle(), &imageInfo, nullptr, &mDepthImage));

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(mDevice.handle(), mDepthImage, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex
            = mDevice.findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VK_CHECK(vkAllocateMemory(mDevice.handle(), &allocInfo, nullptr, &mDepthImageMemory));
        VK_CHECK(vkBindImageMemory(mDevice.handle(), mDepthImage, mDepthImageMemory, 0));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = mDepthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(mDevice.handle(), &viewInfo, nullptr, &mDepthImageView));
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

    VkFormat Swapchain::findDepthFormat() const
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(mDevice.physical(), VK_FORMAT_D32_SFLOAT, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return VK_FORMAT_D32_SFLOAT;

        vkGetPhysicalDeviceFormatProperties(mDevice.physical(), VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return VK_FORMAT_D32_SFLOAT_S8_UINT;

        vkGetPhysicalDeviceFormatProperties(mDevice.physical(), VK_FORMAT_D24_UNORM_S8_UINT, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return VK_FORMAT_D24_UNORM_S8_UINT;

        throw std::runtime_error("Failed to find supported depth format");
    }
}

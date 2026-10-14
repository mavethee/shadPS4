// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "vulkan/vulkan.hpp"

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        UNREACHABLE();
    }
}

bool IsViewTypeCompatible(AmdGpu::ImageType view_type, AmdGpu::ImageType image_type) {
    switch (view_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
    case AmdGpu::ImageType::Cube:
        return image_type == AmdGpu::ImageType::Color1D ||
               image_type == AmdGpu::ImageType::Color2D || image_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return image_type == AmdGpu::ImageType::Color3D;
    default:
        UNREACHABLE();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    const u32 num_levels = std::max<u32>(image.NumLevels(), 1);
    const u32 num_layers = std::max<u32>(image.NumLayers(), 1);
    range.base.level = std::min<u32>(image.base_level, num_levels - 1);
    range.base.layer = std::min<u32>(image.base_array, num_layers - 1);
    range.extent.levels =
        std::clamp<u32>(image.NumViewLevels(desc.is_array), 1, num_levels - range.base.level);
    range.extent.layers =
        std::clamp<u32>(image.NumViewLayers(desc.is_array), 1, num_layers - range.base.layer);
    type = image.GetViewType(desc.is_array);
    min_lod = static_cast<u32>(image.min_lod);
    if (!is_storage) {
        mapping = image.DstSelect();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

ImageView::ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info_,
                     const Image& image)
    : info{info_} {
    const u32 max_image_levels = std::max<u32>(image.info.resources.levels, 1);
    const u32 base_level = std::min<u32>(info.range.base.level, max_image_levels - 1);
    const u32 level_count =
        std::clamp<u32>(info.range.extent.levels, 1, max_image_levels - base_level);

    const auto view_type = ConvertImageViewType(info.type);
    const u32 max_image_layers = std::max<u32>(image.info.resources.layers, 1);
    const u32 base_layer = std::min<u32>(info.range.base.layer, max_image_layers - 1);
    u32 layer_count =
        std::clamp<u32>(info.range.extent.layers, 1, max_image_layers - base_layer);
    if (view_type == vk::ImageViewType::e1D || view_type == vk::ImageViewType::e2D ||
        view_type == vk::ImageViewType::e3D) {
        layer_count = 1;
    }

    vk::ImageViewUsageCreateInfo usage_ci{.usage = image.usage_flags};
    if (!info.is_storage) {
        usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (info.min_lod != 0 && instance.IsImageViewMinLodSupported()) {
        const float last_level = static_cast<float>(base_level + level_count - 1);
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.f, last_level);
        usage_ci.pNext = &min_lod_ci;
    }
    // When sampling D32/D16 texture from shader, the T# specifies R32/R16 format so adjust it.
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = image.aspect_mask;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil &&
        Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eStencil;
    }

    const auto components = info.mapping == AmdGpu::IdentityMapping
                                ? vk::ComponentMapping{}
                                : Vulkan::LiverpoolToVK::ComponentMapping(info.mapping);

    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = view_type,
        .format = instance.GetSupportedFormat(format, image.format_features),
        .components = components,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = base_level,
            .levelCount = level_count,
            .baseArrayLayer = base_layer,
            .layerCount = layer_count,
        },
    };
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }

    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    if (view_result != vk::Result::eSuccess) {
        LOG_CRITICAL(
            Render_Vulkan,
            "Failed to create image view: result={} type={} fmt={} baseMip={} mips={} baseLayer={} layers={}",
            vk::to_string(view_result), vk::to_string(view_type), vk::to_string(format),
            base_level, level_count, base_layer, layer_count);
    }
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    image_view = std::move(view);

    const auto view_aspect = aspect & vk::ImageAspectFlagBits::eDepth     ? "Depth"
                             : aspect & vk::ImageAspectFlagBits::eStencil ? "Stencil"
                                                                          : "Color";
    Vulkan::SetObjectName(
        instance.GetDevice(), *image_view, "ImageView {}x{}x{} {:#x}:{:#x} {}:{} {}:{} ({})",
        image.info.size.width, image.info.size.height, image.info.size.depth,
        image.info.guest_address, image.info.guest_size, base_level,
        base_level + level_count - 1, base_layer,
        base_layer + layer_count - 1, view_aspect);
}

ImageView::~ImageView() = default;

} // namespace VideoCore

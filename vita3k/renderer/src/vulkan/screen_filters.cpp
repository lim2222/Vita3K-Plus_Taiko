// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "renderer/vulkan/screen_filters.h"
#include "renderer/vulkan/screen_renderer.h"

#include "renderer/vulkan/state.h"

namespace renderer::vulkan {

ScreenFilter::ScreenFilter(ScreenRenderer &screen_renderer)
    : screen(screen_renderer) {}

struct screen_vertex {
    float pos[3];
    float uv[2];
};

static constexpr size_t screen_vertex_size = sizeof(screen_vertex);
static constexpr uint32_t screen_vertex_count = 4;

using screen_vertices_t = screen_vertex[screen_vertex_count];

// Computes the rectangle the Vita display occupies in the swapchain, in
// top-origin pixels. This is the single source of truth for where the game
// screen is: the screen filters draw there, the animation background fills
// the margins around it and the touch mapping uses it.
DisplayRect compute_display_rect(const VKState &state, const vk::Extent2D &extent) {
    DisplayRect rect{};

    const float fb_w = static_cast<float>(extent.width);
    const float fb_h = static_cast<float>(extent.height);
    if (fb_h <= 0.0f)
        return rect;

    const float window_aspect = fb_w / fb_h;
    constexpr float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
    constexpr float portrait_aspect = 4.0f / 3.0f; // 4:3 for portrait mode
    const bool pixel_perfect = state.fullscreen_hd_res_pixel_perfect && state.fullscreen
        && !(extent.width % DEFAULT_RES_WIDTH)
        && !(extent.height % (DEFAULT_RES_HEIGHT - 4));

    // Determine effective aspect ratio based on screen orientation
    // 0=Auto (follow device), 1=Portrait (4:3, top), 2=Landscape (Vita native)
    // For Auto mode: check if device is in portrait orientation (rotation 0 or 2)
    const bool is_portrait_mode = (state.screen_orientation == 1)
        || (state.screen_orientation == 0 && (state.device_native_rotation == 0 || state.device_native_rotation == 2));
    const float target_aspect = is_portrait_mode ? portrait_aspect : vita_aspect;

    if (state.stretch_the_display_area && !pixel_perfect) {
        // Match the aspect ratio to the screen size.
        rect.x = 0.0f;
        rect.y = 0.0f;
        rect.width = fb_w;
        rect.height = fb_h;
    } else if ((window_aspect > target_aspect) && !pixel_perfect) {
        // Window is wide. Pin top and bottom.
        rect.width = fb_h * target_aspect;
        rect.height = fb_h;
        rect.x = (fb_w - rect.width) / 2.0f;
        rect.y = 0.0f;
    } else {
        // Window is tall. Pin left and right.
        rect.width = fb_w;
        rect.height = fb_w / target_aspect;
        rect.x = 0.0f;
        rect.y = is_portrait_mode ? state.portrait_y_offset : (fb_h - rect.height) / 2.0f;
    }

    return rect;
}

// Graphics pipeline drawing a screen_vertex quad with a dynamic viewport and
// scissor, shared by the game screen filters and the animation background.
static vk::Pipeline create_quad_graphics_pipeline(vk::Device device, vk::ShaderModule vertex_shader, vk::ShaderModule fragment_shader,
    vk::PipelineLayout pipeline_layout, vk::RenderPass render_pass) {
    vk::PipelineShaderStageCreateInfo vert_info{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = vertex_shader,
        .pName = "main"
    };
    vk::PipelineShaderStageCreateInfo frag_info{
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = fragment_shader,
        .pName = "main"
    };
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = { vert_info, frag_info };

    vk::VertexInputBindingDescription binding_descr{
        .binding = 0,
        .stride = screen_vertex_size,
        .inputRate = vk::VertexInputRate::eVertex
    };
    std::array<vk::VertexInputAttributeDescription, 2> attr_descr;
    // pos
    attr_descr[0] = vk::VertexInputAttributeDescription{
        .location = 0,
        .binding = 0,
        .format = vk::Format::eR32G32B32Sfloat,
        .offset = offsetof(screen_vertex, pos)
    };
    // uv
    attr_descr[1] = vk::VertexInputAttributeDescription{
        .location = 1,
        .binding = 0,
        .format = vk::Format::eR32G32Sfloat,
        .offset = offsetof(screen_vertex, uv)
    };
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding_descr);
    vertex_input.setVertexAttributeDescriptions(attr_descr);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleStrip
    };
    vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1
    };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vkutil::default_color_mask
    };
    vk::PipelineColorBlendStateCreateInfo color_blending{};
    color_blending.setAttachments(blend_attachment);

    // dynamic scissor and viewport are used, in case the extent changes
    static vk::DynamicState dynamic_states[] = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    dynamic_info.setDynamicStates(dynamic_states);

    vk::GraphicsPipelineCreateInfo pipeline_info{
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0
    };
    pipeline_info.setStages(shader_stages);

    const auto result = device.createGraphicsPipeline(VK_NULL_HANDLE, pipeline_info);
    if (result.result != vk::Result::eSuccess) {
        LOG_CRITICAL("Failed to create pipeline.");
    }
    return result.value;
}

ScreenFilter::~ScreenFilter() {
    // the derived class already waited and destroyed its own objects; the
    // shared animation background drawing needs its own wait before freeing
    if (!background_objects_created)
        return;
    vk::Device device = screen.state.device;
    screen.state.wait_gpu_idle();
    device.destroy(background_pipeline);
    device.destroy(background_post_pipeline);
    device.destroy(background_pipeline_layout);
    device.destroy(background_descriptor_pool); // frees the sets as well
    device.destroy(background_descriptor_set_layout);
    device.destroy(background_fragment_shader);
    device.destroy(background_vertex_shader);
    device.destroy(background_sampler);
    // background_vao cleans itself up
}

void ScreenFilter::ensure_background_objects() {
    if (background_objects_created)
        return;
    vk::Device device = screen.state.device;

    fs::path builtin_shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";
    background_vertex_shader = vkutil::load_shader(device, builtin_shaders_path / "render_main.vert.spv");
    background_fragment_shader = vkutil::load_shader(device, builtin_shaders_path / "render_main.frag.spv");

    // one combined image sampler binding, refreshed per frame
    vk::DescriptorSetLayoutBinding sampler_layout_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    vk::DescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.setBindings(sampler_layout_binding);
    background_descriptor_set_layout = device.createDescriptorSetLayout(descriptor_info);

    // one descriptor set per swapchain image, like the game screen quad
    vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = screen.swapchain_size
    };
    vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = screen.swapchain_size,
    };
    pool_info.setPoolSizes(pool_size);
    background_descriptor_pool = device.createDescriptorPool(pool_info);

    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = background_descriptor_pool,
    };
    std::vector<vk::DescriptorSetLayout> descr_set_layouts(screen.swapchain_size, background_descriptor_set_layout);
    descr_set_info.setSetLayouts(descr_set_layouts);
    background_descriptor_sets = device.allocateDescriptorSets(descr_set_info);

    // no push constants: the plain render_main shaders sample binding 0 only
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(background_descriptor_set_layout);
    background_pipeline_layout = device.createPipelineLayout(layout_info);

    // one pipeline per render pass, they are not compatible with each other
    background_pipeline = create_background_pipeline(screen.default_render_pass);
    background_post_pipeline = create_background_pipeline(screen.post_filter_render_pass);

    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    background_sampler = device.createSampler(sampler_info);

    // one fullscreen quad with identity uvs, shared by all swapchain images
    // and never rewritten
    background_vao.size = sizeof(screen_vertices_t);
    background_vao.init_buffer(vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
    background_vao_written = false;

    background_objects_created = true;
}

vk::Pipeline ScreenFilter::create_background_pipeline(vk::RenderPass render_pass) {
    return create_quad_graphics_pipeline(screen.state.device, background_vertex_shader, background_fragment_shader,
        background_pipeline_layout, render_pass);
}

void ScreenFilter::prepare_background() {
    ensure_background_objects();

    // the background quad never changes, write it once. This runs outside the
    // render pass: vkCmdUpdateBuffer (the fallback for buffers that are not
    // host-mappable) is not allowed inside one.
    if (background_vao_written)
        return;

    const screen_vertices_t background_quad = {
        { { 1.f, -1.f, 0.0f }, { 1.f, 0.f } },
        { { -1.f, -1.f, 0.0f }, { 0.f, 0.f } },
        { { 1.f, 1.f, 0.0f }, { 1.f, 1.f } },
        { { -1.f, 1.f, 0.0f }, { 0.f, 1.f } },
    };
    if (background_vao.mapped_data)
        memcpy(background_vao.mapped_data, &background_quad, sizeof(background_quad));
    else
        screen.current_cmd_buffer.updateBuffer(background_vao.buffer, 0, sizeof(background_quad), &background_quad);
    background_vao_written = true;
}

void ScreenFilter::render_background(vk::ImageView background_img, bool post_processing_pass) {
    if (!background_objects_created || !background_img)
        return; // not prepared (outside the render pass) yet, skip this frame

    // refresh the descriptor set of this swapchain image
    vk::DescriptorImageInfo descr_image_info{
        .sampler = background_sampler,
        .imageView = background_img,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    vk::WriteDescriptorSet write_descr{
        .dstSet = background_descriptor_sets[screen.swapchain_image_idx],
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
    };
    write_descr.setImageInfo(descr_image_info);
    screen.state.device.updateDescriptorSets(write_descr, {});

    // the margins around the game screen, in top-origin pixels like
    // compute_display_rect
    const DisplayRect display = compute_display_rect(screen.state, screen.extent);
    const float fb_w = static_cast<float>(screen.extent.width);
    const float fb_h = static_cast<float>(screen.extent.height);
    const float left = display.x;
    const float right = fb_w - display.x - display.width;
    const float bottom = fb_h - display.y - display.height;

    vk::CommandBuffer cmd_buffer = screen.current_cmd_buffer;
    cmd_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
        post_processing_pass ? background_post_pipeline : background_pipeline);
    cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, background_pipeline_layout, 0,
        background_descriptor_sets[screen.swapchain_image_idx], {});

    const vk::DeviceSize offset = 0;
    cmd_buffer.bindVertexBuffers(0, background_vao.buffer, offset);

    const auto draw_bar = [&](float bar_x, float bar_y, float bar_w, float bar_h) {
        vk::Viewport vk_viewport{
            .x = bar_x,
            .y = bar_y,
            .width = bar_w,
            .height = bar_h,
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        cmd_buffer.setViewport(0, vk_viewport);
        vk::Rect2D vk_scissor{
            .offset = { static_cast<int32_t>(bar_x), static_cast<int32_t>(bar_y) },
            .extent = { static_cast<uint32_t>(bar_w), static_cast<uint32_t>(bar_h) }
        };
        cmd_buffer.setScissor(0, vk_scissor);
        cmd_buffer.draw(4, 1, 0, 0);
    };

    // like PPSSPP: landscape fills both side margins, portrait only draws
    // below the screen and keeps the bar above it black
    if (left > 0.5f || right > 0.5f) {
        if (left > 0.5f)
            draw_bar(0.0f, 0.0f, left, fb_h);
        if (right > 0.5f)
            draw_bar(display.x + display.width, 0.0f, right, fb_h);
    } else if (bottom > 0.5f) {
        draw_bar(0.0f, display.y + display.height, fb_w, bottom);
    }
}

SinglePassScreenFilter::SinglePassScreenFilter(ScreenRenderer &screen)
    : ScreenFilter(screen) {}

SinglePassScreenFilter::~SinglePassScreenFilter() {
    vk::Device device = screen.state.device;
    // this will only happen when the user changes the option in the GUI, we can afford to waitIdle
    screen.state.wait_gpu_idle();
    device.destroy(pipeline);
    device.destroy(pipeline_layout);
    device.destroy(descriptor_pool);
    device.destroy(descriptor_set_layout);
    device.destroy(fragment_shader);
    device.destroy(vertex_shader);

    device.destroy(sampler);
}

void SinglePassScreenFilter::create_layout_sync() {
    vk::Device device = screen.state.device;

    vk::DescriptorSetLayoutBinding sampler_layout_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    vk::DescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.setBindings(sampler_layout_binding);
    descriptor_set_layout = device.createDescriptorSetLayout(descriptor_info);

    vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = screen.swapchain_size
    };
    vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = screen.swapchain_size,
    };
    pool_info.setPoolSizes(pool_size);
    descriptor_pool = device.createDescriptorPool(pool_info);

    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool,
    };
    std::vector<vk::DescriptorSetLayout> descr_set_layouts(screen.swapchain_size, descriptor_set_layout);
    descr_set_info.setSetLayouts(descr_set_layouts);
    descriptor_sets = device.allocateDescriptorSets(descr_set_info);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(descriptor_set_layout);
    // add push constant for fxaa pipeline, not used by the normal pipeline
    vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = 2 * sizeof(float),
    };
    layout_info.setPushConstantRanges(push_constant);
    pipeline_layout = device.createPipelineLayout(layout_info);

    // create vao
    vao.size = sizeof(screen_vertices_t) * screen.swapchain_size;
    vao.init_buffer(vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
    if (!vao.mapped_data)
        LOG_ERROR("screen filter vao is not host-mappable on this device; falling back to updateBuffer");

    // create and zero-fill uvs
    last_uvs.resize(screen.swapchain_size);
    std::fill(last_uvs.begin(), last_uvs.end(), std::array<float, 4>());
}

void SinglePassScreenFilter::create_graphics_pipeline() {
    // create shader modules

    fs::path builtin_shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";
    const auto vertex_shader_path = builtin_shaders_path / get_vertex_name();
    const auto fragment_shader_path = builtin_shaders_path / get_fragment_name();

    vertex_shader = vkutil::load_shader(screen.state.device, vertex_shader_path);
    fragment_shader = vkutil::load_shader(screen.state.device, fragment_shader_path);

    pipeline = create_quad_graphics_pipeline(screen.state.device, vertex_shader, fragment_shader,
        pipeline_layout, screen.default_render_pass);
}

std::string_view SinglePassScreenFilter::get_vertex_name() {
    return "render_main.vert.spv";
}

std::string_view SinglePassScreenFilter::get_fragment_name() {
    return "render_main.frag.spv";
}

void SinglePassScreenFilter::init() {
    create_layout_sync();
    create_graphics_pipeline();
    this->sampler = create_sampler();
}

void SinglePassScreenFilter::render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (is_pre_renderpass) {
        std::array<float, 4> uvs = {
            viewport.offset_x / (float)viewport.texture_width,
            viewport.offset_y / (float)viewport.texture_height,
            (viewport.offset_x + viewport.width) / (float)(viewport.texture_width),
            (viewport.offset_y + viewport.height) / (float)(viewport.texture_height)
        };

        // write the quad every frame via the mapped pointer
        const bool host_write = vao.mapped_data != nullptr;
        if (host_write || uvs != last_uvs[screen.swapchain_image_idx]) {
            screen_vertices_t vertex_buffer_data = {
                { { 1.f, -1.f, 0.0f }, { 1.f, 0.f } },
                { { -1.f, -1.f, 0.0f }, { 0.f, 0.f } },
                { { 1.f, 1.f, 0.0f }, { 1.f, 1.f } },
                { { -1.f, 1.f, 0.0f }, { 0.f, 1.f } },
            };
            vertex_buffer_data[0].uv[0] = uvs[2];
            vertex_buffer_data[0].uv[1] = uvs[1];

            vertex_buffer_data[1].uv[0] = uvs[0];
            vertex_buffer_data[1].uv[1] = uvs[1];

            vertex_buffer_data[2].uv[0] = uvs[2];
            vertex_buffer_data[2].uv[1] = uvs[3];

            vertex_buffer_data[3].uv[0] = uvs[0];
            vertex_buffer_data[3].uv[1] = uvs[3];

            if (host_write)
                memcpy(static_cast<uint8_t *>(vao.mapped_data) + screen.swapchain_image_idx * sizeof(screen_vertices_t),
                    &vertex_buffer_data, sizeof(screen_vertices_t));
            else
                screen.current_cmd_buffer.updateBuffer(vao.buffer, screen.swapchain_image_idx * sizeof(screen_vertices_t), sizeof(screen_vertices_t), &vertex_buffer_data);
            last_uvs[screen.swapchain_image_idx] = uvs;
        }

        return;
    }

    {
        // update descriptor set
        vk::DescriptorImageInfo descr_image_info{
            .sampler = sampler,
            .imageView = src_img,
            .imageLayout = src_layout,
        };
        vk::WriteDescriptorSet write_descr{
            .dstSet = descriptor_sets[screen.swapchain_image_idx],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        };
        write_descr.setImageInfo(descr_image_info);
        screen.state.device.updateDescriptorSets(write_descr, {});

        // set viewport and scissor
        vk::Rect2D vk_scissor{
            .offset = { 0, 0 },
            .extent = screen.extent
        };
        screen.current_cmd_buffer.setScissor(0, vk_scissor);
        const DisplayRect display = compute_display_rect(screen.state, screen.extent);
        vk::Viewport vk_viewport{
            .x = display.x,
            .y = display.y,
            .width = display.width,
            .height = display.height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        screen.current_cmd_buffer.setViewport(0, vk_viewport);
    }

    {
        vk::DeviceSize offset = screen.swapchain_image_idx * sizeof(screen_vertices_t);
        screen.current_cmd_buffer.bindVertexBuffers(0, vao.buffer, offset);

        screen.current_cmd_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        screen.current_cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, descriptor_sets[screen.swapchain_image_idx], {});

        std::array<float, 2> inv_size = { 1.f / viewport.texture_width, 1.f / viewport.texture_height };
        screen.current_cmd_buffer.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, 2 * sizeof(float), inv_size.data());

        screen.current_cmd_buffer.draw(4, 1, 0, 0);
    }
}

vk::Sampler NearestScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

vk::Sampler BilinearScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view BicubicScreenFilter::get_fragment_name() {
    return "render_main_bicubic.frag.spv";
}

vk::Sampler BicubicScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view FXAAScreenFilter::get_fragment_name() {
    return "render_main_fxaa.frag.spv";
}

vk::Sampler FXAAScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

struct EasuConstant {
    Viewport viewport;
    vk::Extent2D output_size;
};

struct RcasConstant {
    vk::Extent2D offset;
    float sharpening;
};

FSRScreenFilter::~FSRScreenFilter() {
    vk::Device device = screen.state.device;
    screen.state.wait_gpu_idle();

    device.destroy(sampler);

    device.destroy(pipeline_easu);
    device.destroy(pipeline_rcas);
    device.destroy(pipeline_layout_easu);
    device.destroy(pipeline_layout_rcas);
    device.destroy(descriptor_pool);
    device.destroy(descriptor_set_layout);

    device.destroy(rcas_shader);
    device.destroy(easu_shader);
}

void FSRScreenFilter::init() {
    vk::Device device = screen.state.device;

    // create sampler
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = device.createSampler(sampler_info);

    fs::path builtin_shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";

    const auto easu_shader_path = builtin_shaders_path / "fsr_filter_easu.comp.spv";
    const auto frcas_shader_path = builtin_shaders_path / "fsr_filter_rcas.comp.spv";

    easu_shader = vkutil::load_shader(screen.state.device, easu_shader_path);
    rcas_shader = vkutil::load_shader(screen.state.device, frcas_shader_path);

    std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings = {
        // src img
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute },
        // dst img
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute },
        // src img sampler
        vk::DescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler },
    };

    vk::DescriptorSetLayoutCreateInfo layout_create_info{};
    layout_create_info.setBindings(layout_bindings);
    descriptor_set_layout = device.createDescriptorSetLayout(layout_create_info);

    std::array<vk::DescriptorPoolSize, 3> pool_sizes{
        vk::DescriptorPoolSize{
            .type = vk::DescriptorType::eSampledImage,
            .descriptorCount = screen.swapchain_size * 2 },
        vk::DescriptorPoolSize{
            .type = vk::DescriptorType::eStorageImage,
            .descriptorCount = screen.swapchain_size * 2 },
        vk::DescriptorPoolSize{
            .type = vk::DescriptorType::eSampler,
            .descriptorCount = screen.swapchain_size * 2 },
    };
    vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = screen.swapchain_size * 2,
    };
    pool_info.setPoolSizes(pool_sizes);
    descriptor_pool = device.createDescriptorPool(pool_info);

    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool,
    };
    std::vector<vk::DescriptorSetLayout> descr_set_layouts(screen.swapchain_size * 2, descriptor_set_layout);
    descr_set_info.setSetLayouts(descr_set_layouts);
    descriptor_sets = device.allocateDescriptorSets(descr_set_info);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(descriptor_set_layout);
    vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(EasuConstant),
    };
    layout_info.setPushConstantRanges(push_constant);
    pipeline_layout_easu = device.createPipelineLayout(layout_info);

    push_constant.size = sizeof(RcasConstant);
    pipeline_layout_rcas = device.createPipelineLayout(layout_info);

    // create easu and rcas pipelines
    vk::ComputePipelineCreateInfo compute_info{
        .stage = {
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = easu_shader,
            .pName = "main" },
        .layout = pipeline_layout_easu
    };
    auto result = device.createComputePipeline(nullptr, compute_info);
    if (result.result != vk::Result::eSuccess)
        LOG_ERROR("Failed to create compute pipeline");
    pipeline_easu = result.value;

    compute_info.stage.module = rcas_shader;
    compute_info.layout = pipeline_layout_rcas;
    result = device.createComputePipeline(nullptr, compute_info);
    if (result.result != vk::Result::eSuccess)
        LOG_ERROR("Failed to create compute pipeline");
    pipeline_rcas = result.value;

    // create intermediate images
    intermediate_images.resize(screen.swapchain_size);
    for (auto &img : intermediate_images)
        img.format = vk::Format::eR8G8B8A8Unorm;

    on_resize();
}

void FSRScreenFilter::on_resize() {
    // compute the output rectangle, shared with the other screen filters
    // and the animation background margins
    const DisplayRect display = compute_display_rect(screen.state, screen.extent);
    output_size.width = static_cast<uint32_t>(std::round(display.width));
    output_size.height = static_cast<uint32_t>(std::round(display.height));
    output_offset.width = static_cast<uint32_t>(std::round(display.x));
    output_offset.height = static_cast<uint32_t>(std::round(display.y));

    // recreate the intermediate images
    for (auto &img : intermediate_images) {
        img.destroy();
        img.width = output_size.width;
        img.height = output_size.height;
        img.init_image(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    }

    // update the descriptor sets (except the first sampler image as it is not fixed)
    std::vector<vk::DescriptorImageInfo> descr_images(screen.swapchain_size * 3);
    std::vector<vk::WriteDescriptorSet> write_descr(screen.swapchain_size * 3);
    for (size_t i = 0; i < write_descr.size(); i++) {
        descr_images[i].imageView = intermediate_images[i / 3].view;
        write_descr[i].setImageInfo(descr_images[i]);
    }

    if (!screen.swapchain_has_storage) {
        rcas_images.resize(screen.swapchain_size);
        for (auto &img : rcas_images) {
            img.destroy();
            img.width = output_size.width;
            img.height = output_size.height;
            img.format = vk::Format::eR8G8B8A8Unorm;
            img.init_image(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc);
        }
    }

    for (uint32_t i = 0; i < screen.swapchain_size; i++) {
        // easu dst
        descr_images[i * 3].imageLayout = vk::ImageLayout::eGeneral;
        write_descr[i * 3]
            .setDstSet(descriptor_sets[i * 2])
            .setDstBinding(2)
            .setDescriptorType(vk::DescriptorType::eStorageImage);
        // rcas src
        descr_images[i * 3 + 1].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        write_descr[i * 3 + 1]
            .setDstSet(descriptor_sets[i * 2 + 1])
            .setDstBinding(1)
            .setDescriptorType(vk::DescriptorType::eSampledImage);
        // rcas dst
        descr_images[i * 3 + 2]
            .setImageView(screen.swapchain_has_storage ? screen.swapchain_views[i] : rcas_images[i].view)
            .setImageLayout(vk::ImageLayout::eGeneral);
        write_descr[i * 3 + 2]
            .setDstSet(descriptor_sets[i * 2 + 1])
            .setDstBinding(2)
            .setDescriptorType(vk::DescriptorType::eStorageImage);
    }
    screen.state.device.updateDescriptorSets(write_descr, {});
}

void FSRScreenFilter::render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (!is_pre_renderpass)
        // we are using compute shaders
        return;

    // the output rectangle can change at runtime without a swapchain rebuild
    // (portrait y offset slider, orientation flip): recompute the
    // intermediate images when it does. We are outside the render pass and
    // rebuilding them requires the gpu to be idle.
    const DisplayRect display = compute_display_rect(screen.state, screen.extent);
    const uint32_t expected_width = static_cast<uint32_t>(std::round(display.width));
    const uint32_t expected_height = static_cast<uint32_t>(std::round(display.height));
    const uint32_t expected_x = static_cast<uint32_t>(std::round(display.x));
    const uint32_t expected_y = static_cast<uint32_t>(std::round(display.y));
    if (expected_width != output_size.width || expected_height != output_size.height
        || expected_x != output_offset.width || expected_y != output_offset.height) {
        screen.state.wait_gpu_idle();
        on_resize();
    }

    // update descriptor set, (only the easu src)
    vk::DescriptorImageInfo descr_image_info{
        .imageView = src_img,
        .imageLayout = src_layout,
    };
    vk::WriteDescriptorSet write_descr{
        .dstSet = descriptor_sets[2 * screen.swapchain_image_idx],
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
    };
    write_descr.setImageInfo(descr_image_info);
    screen.state.device.updateDescriptorSets(write_descr, {});

    vk::CommandBuffer cmd_buffer = screen.current_cmd_buffer;
    // first, make a barrier to make sure we can write to the intermediate texture
    // we don't care about the previous content
    intermediate_images[screen.swapchain_image_idx].transition_to_discard(cmd_buffer, vkutil::ImageLayout::StorageImage);

    // upscaling pass
    cmd_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_easu);
    cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout_easu, 0, descriptor_sets[2 * screen.swapchain_image_idx], {});
    // push the viewport to the shader
    EasuConstant easu_constant{
        .viewport = viewport,
        .output_size = output_size
    };
    cmd_buffer.pushConstants(pipeline_layout_easu, vk::ShaderStageFlagBits::eCompute, 0, sizeof(EasuConstant), &easu_constant);
    const int dispatch_x = (output_size.width + 15) / 16;
    const int dispatch_y = (output_size.height + 15) / 16;
    cmd_buffer.dispatch(dispatch_x, dispatch_y, 1);

    // meanwhile, we need to clear the swapchain surface
    vk::ImageMemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = screen.swapchain_images[screen.swapchain_image_idx],
        .subresourceRange = vkutil::color_subresource_range
    };
    cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(), {}, {}, barrier);

    vk::ClearColorValue clear_color{ std::array<float, 4>({ 0.0f, 0.0f, 0.0f, 0.0f }) };
    cmd_buffer.clearColorImage(screen.swapchain_images[screen.swapchain_image_idx], vk::ImageLayout::eTransferDstOptimal, clear_color, vkutil::color_subresource_range);

    // then transition the read texture to sampled // wait for the previous compute shader to be done
    intermediate_images[screen.swapchain_image_idx].transition_to(cmd_buffer, vkutil::ImageLayout::SampledImage);

    if (screen.swapchain_has_storage) {
        // also transition the swapchain image to general
        barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = screen.swapchain_images[screen.swapchain_image_idx],
            .subresourceRange = vkutil::color_subresource_range
        };
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlags(), {}, {}, barrier);
    } else {
        // rcas writes into its own image instead (the swapchain has no storage usage) so swapchain stays transfer dst
        rcas_images[screen.swapchain_image_idx].transition_to_discard(cmd_buffer, vkutil::ImageLayout::StorageImage);
    }

    // sharpening pass
    cmd_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_rcas);
    cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout_rcas, 0, descriptor_sets[2 * screen.swapchain_image_idx + 1], {});
    RcasConstant rcas_constant{
        .offset = screen.swapchain_has_storage ? output_offset : vk::Extent2D{},
        // some default value for sharpening
        .sharpening = 0.2f
    };
    cmd_buffer.pushConstants(pipeline_layout_rcas, vk::ShaderStageFlagBits::eCompute, 0, sizeof(RcasConstant), &rcas_constant);
    cmd_buffer.dispatch(dispatch_x, dispatch_y, 1);

    if (!screen.swapchain_has_storage) {
        vkutil::Image &rcas_img = rcas_images[screen.swapchain_image_idx];
        rcas_img.transition_to(cmd_buffer, vkutil::ImageLayout::TransferSrc);

        const auto off_x = static_cast<int32_t>(output_offset.width);
        const auto off_y = static_cast<int32_t>(output_offset.height);
        const auto out_w = static_cast<int32_t>(output_size.width);
        const auto out_h = static_cast<int32_t>(output_size.height);
        vk::ImageBlit blit_region{
            .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .srcOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ out_w, out_h, 1 } },
            .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .dstOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ off_x, off_y, 0 }, vk::Offset3D{ off_x + out_w, off_y + out_h, 1 } }
        };
        cmd_buffer.blitImage(rcas_img.image, vk::ImageLayout::eTransferSrcOptimal,
            screen.swapchain_images[screen.swapchain_image_idx], vk::ImageLayout::eTransferDstOptimal,
            blit_region, vk::Filter::eNearest);

        barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = screen.swapchain_images[screen.swapchain_image_idx],
            .subresourceRange = vkutil::color_subresource_range
        };
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::DependencyFlags(), {}, {}, barrier);
    }

    // the barrier for the render pass will be handled by the renderpass external dependencies
}

} // namespace renderer::vulkan

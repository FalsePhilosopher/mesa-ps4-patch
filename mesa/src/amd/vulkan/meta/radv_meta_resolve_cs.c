/*
 * Copyright © 2016 Dave Airlie
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>

#include "nir/nir_builder.h"
#include "nir/nir_format_convert.h"

#include "radv_entrypoints.h"
#include "radv_formats.h"
#include "radv_meta.h"
#include "sid.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"
#include "vk_shader_module.h"

static nir_def *
radv_meta_build_resolve_srgb_conversion(nir_builder *b, nir_def *input)
{
   unsigned i;
   nir_def *comp[4];
   for (i = 0; i < 3; i++)
      comp[i] = nir_format_linear_to_srgb(b, nir_channel(b, input, i));
   comp[3] = nir_channels(b, input, 1 << 3);
   return nir_vec(b, comp, 4);
}

static nir_shader *
build_resolve_compute_shader(struct radv_device *dev, bool is_integer, bool is_srgb, int samples)
{
   enum glsl_base_type img_base_type = is_integer ? GLSL_TYPE_UINT : GLSL_TYPE_FLOAT;
   const struct glsl_type *sampler_type = glsl_sampler_type(GLSL_SAMPLER_DIM_MS, false, false, img_base_type);
   const struct glsl_type *img_type = glsl_image_type(GLSL_SAMPLER_DIM_2D, false, img_base_type);
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_resolve_cs-%d-%s", samples,
                                         is_integer ? "int" : (is_srgb ? "srgb" : "float"));
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;

   nir_variable *input_img = nir_variable_create(b.shader, nir_var_uniform, sampler_type, "s_tex");
   input_img->data.descriptor_set = 0;
   input_img->data.binding = 0;

   nir_variable *output_img = nir_variable_create(b.shader, nir_var_image, img_type, "out_img");
   output_img->data.descriptor_set = 0;
   output_img->data.binding = 1;

   nir_def *global_id = get_global_ids(&b, 2);

   nir_def *src_offset = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 0), .range = 8);
   nir_def *dst_offset = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 8), .range = 16);

   nir_def *src_coord = nir_iadd(&b, global_id, src_offset);
   nir_def *dst_coord = nir_iadd(&b, global_id, dst_offset);

   nir_variable *color = nir_local_variable_create(b.impl, glsl_vec4_type(), "color");

   radv_meta_build_resolve_shader_core(dev, &b, is_integer, samples, input_img, color, src_coord);

   nir_def *outval = nir_load_var(&b, color);
   if (is_srgb)
      outval = radv_meta_build_resolve_srgb_conversion(&b, outval);

   nir_def *img_coord = nir_vec4(&b, nir_channel(&b, dst_coord, 0), nir_channel(&b, dst_coord, 1), nir_undef(&b, 1, 32),
                                 nir_undef(&b, 1, 32));

   nir_image_deref_store(&b, &nir_build_deref_var(&b, output_img)->def, img_coord, nir_undef(&b, 1, 32), outval,
                         nir_imm_int(&b, 0), .image_dim = GLSL_SAMPLER_DIM_2D);
   return b.shader;
}

enum {
   DEPTH_RESOLVE,
   STENCIL_RESOLVE,
};

static const char *
get_resolve_mode_str(VkResolveModeFlagBits resolve_mode)
{
   switch (resolve_mode) {
   case VK_RESOLVE_MODE_SAMPLE_ZERO_BIT:
      return "zero";
   case VK_RESOLVE_MODE_AVERAGE_BIT:
      return "average";
   case VK_RESOLVE_MODE_MIN_BIT:
      return "min";
   case VK_RESOLVE_MODE_MAX_BIT:
      return "max";
   default:
      unreachable("invalid resolve mode");
   }
}

static nir_shader *
build_depth_stencil_resolve_compute_shader(struct radv_device *dev, int samples, int index,
                                           VkResolveModeFlagBits resolve_mode)
{
   enum glsl_base_type img_base_type = index == DEPTH_RESOLVE ? GLSL_TYPE_FLOAT : GLSL_TYPE_UINT;
   const struct glsl_type *sampler_type = glsl_sampler_type(GLSL_SAMPLER_DIM_MS, false, true, img_base_type);
   const struct glsl_type *img_type = glsl_image_type(GLSL_SAMPLER_DIM_2D, true, img_base_type);

   nir_builder b =
      radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_resolve_cs_%s-%s-%d",
                            index == DEPTH_RESOLVE ? "depth" : "stencil", get_resolve_mode_str(resolve_mode), samples);
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;

   nir_variable *input_img = nir_variable_create(b.shader, nir_var_uniform, sampler_type, "s_tex");
   input_img->data.descriptor_set = 0;
   input_img->data.binding = 0;

   nir_variable *output_img = nir_variable_create(b.shader, nir_var_image, img_type, "out_img");
   output_img->data.descriptor_set = 0;
   output_img->data.binding = 1;

   nir_def *global_id = get_global_ids(&b, 3);

   nir_def *offset = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 0), .range = 8);

   nir_def *resolve_coord = nir_iadd(&b, nir_trim_vector(&b, global_id, 2), offset);

   nir_def *img_coord =
      nir_vec3(&b, nir_channel(&b, resolve_coord, 0), nir_channel(&b, resolve_coord, 1), nir_channel(&b, global_id, 2));

   nir_deref_instr *input_img_deref = nir_build_deref_var(&b, input_img);
   nir_def *outval = nir_txf_ms_deref(&b, input_img_deref, img_coord, nir_imm_int(&b, 0));

   if (resolve_mode != VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) {
      for (int i = 1; i < samples; i++) {
         nir_def *si = nir_txf_ms_deref(&b, input_img_deref, img_coord, nir_imm_int(&b, i));

         switch (resolve_mode) {
         case VK_RESOLVE_MODE_AVERAGE_BIT:
            assert(index == DEPTH_RESOLVE);
            outval = nir_fadd(&b, outval, si);
            break;
         case VK_RESOLVE_MODE_MIN_BIT:
            if (index == DEPTH_RESOLVE)
               outval = nir_fmin(&b, outval, si);
            else
               outval = nir_umin(&b, outval, si);
            break;
         case VK_RESOLVE_MODE_MAX_BIT:
            if (index == DEPTH_RESOLVE)
               outval = nir_fmax(&b, outval, si);
            else
               outval = nir_umax(&b, outval, si);
            break;
         default:
            unreachable("invalid resolve mode");
         }
      }

      if (resolve_mode == VK_RESOLVE_MODE_AVERAGE_BIT)
         outval = nir_fdiv_imm(&b, outval, samples);
   }

   nir_def *coord = nir_vec4(&b, nir_channel(&b, img_coord, 0), nir_channel(&b, img_coord, 1),
                             nir_channel(&b, img_coord, 2), nir_undef(&b, 1, 32));
   nir_image_deref_store(&b, &nir_build_deref_var(&b, output_img)->def, coord, nir_undef(&b, 1, 32), outval,
                         nir_imm_int(&b, 0), .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = true);
   return b.shader;
}

static VkResult
create_layout(struct radv_device *device, VkPipelineLayout *layout_out)
{
   enum radv_meta_object_key_type key = RADV_META_OBJECT_KEY_RESOLVE_CS;

   const VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };

   const VkDescriptorSetLayoutCreateInfo desc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
      .bindingCount = 2,
      .pBindings = bindings,
   };

   const VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = 16,
   };

   return vk_meta_get_pipeline_layout(&device->vk, &device->meta_state.device, &desc_info, &pc_range, &key, sizeof(key),
                                      layout_out);
}

struct radv_resolve_color_cs_key {
   enum radv_meta_object_key_type type;
   bool is_integer;
   bool is_srgb;
   uint8_t samples;
};

static VkResult
get_color_resolve_pipeline(struct radv_device *device, struct radv_image_view *src_iview, VkPipeline *pipeline_out,
                           VkPipelineLayout *layout_out)
{
   const bool is_integer = vk_format_is_int(src_iview->vk.format);
   const bool is_srgb = vk_format_is_srgb(src_iview->vk.format);
   uint32_t samples = src_iview->image->vk.samples;
   struct radv_resolve_color_cs_key key;
   VkResult result;

   result = create_layout(device, layout_out);
   if (result != VK_SUCCESS)
      return result;

   memset(&key, 0, sizeof(key));
   key.type = RADV_META_OBJECT_KEY_RESOLVE_COLOR_CS;
   key.is_integer = is_integer;
   key.is_srgb = is_srgb;
   key.samples = samples;

   VkPipeline pipeline_from_cache = vk_meta_lookup_pipeline(&device->meta_state.device, &key, sizeof(key));
   if (pipeline_from_cache != VK_NULL_HANDLE) {
      *pipeline_out = pipeline_from_cache;
      return VK_SUCCESS;
   }

   nir_shader *cs = build_resolve_compute_shader(device, is_integer, is_srgb, samples);

   const VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = vk_shader_module_handle_from_nir(cs),
      .pName = "main",
      .pSpecializationInfo = NULL,
   };

   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage_info,
      .flags = 0,
      .layout = *layout_out,
   };

   result = vk_meta_create_compute_pipeline(&device->vk, &device->meta_state.device, &pipeline_info, &key, sizeof(key),
                                            pipeline_out);

   ralloc_free(cs);
   return result;
}

static void
emit_resolve(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *src_iview, struct radv_image_view *dst_iview,
             const VkOffset2D *src_offset, const VkOffset2D *dst_offset, const VkExtent2D *resolve_extent)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   VkPipelineLayout layout;
   VkPipeline pipeline;
   VkResult result;

   result = get_color_resolve_pipeline(device, src_iview, &pipeline, &layout);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2,
                                 (VkWriteDescriptorSet[]){{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstBinding = 0,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                           .pImageInfo =
                                                              (VkDescriptorImageInfo[]){
                                                                 {.sampler = VK_NULL_HANDLE,
                                                                  .imageView = radv_image_view_to_handle(src_iview),
                                                                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
                                                              }},
                                                          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstBinding = 1,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                           .pImageInfo = (VkDescriptorImageInfo[]){
                                                              {
                                                                 .sampler = VK_NULL_HANDLE,
                                                                 .imageView = radv_image_view_to_handle(dst_iview),
                                                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                              },
                                                           }}});

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   unsigned push_constants[4] = {
      src_offset->x,
      src_offset->y,
      dst_offset->x,
      dst_offset->y,
   };
   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16,
                              push_constants);
   radv_unaligned_dispatch(cmd_buffer, resolve_extent->width, resolve_extent->height, 1);
}

struct radv_resolve_ds_cs_key {
   enum radv_meta_object_key_type type;
   uint8_t index;
   uint8_t samples;
   VkResolveModeFlagBits resolve_mode;
};

static VkResult
get_depth_stencil_resolve_pipeline(struct radv_device *device, int samples, VkImageAspectFlags aspects,
                                   VkResolveModeFlagBits resolve_mode, VkPipeline *pipeline_out,
                                   VkPipelineLayout *layout_out)

{
   const int index = aspects == VK_IMAGE_ASPECT_DEPTH_BIT ? DEPTH_RESOLVE : STENCIL_RESOLVE;
   struct radv_resolve_ds_cs_key key;
   VkResult result;

   result = create_layout(device, layout_out);
   if (result != VK_SUCCESS)
      return result;

   memset(&key, 0, sizeof(key));
   key.type = RADV_META_OBJECT_KEY_RESOLVE_DS_CS;
   key.index = index;
   key.samples = samples;
   key.resolve_mode = resolve_mode;

   VkPipeline pipeline_from_cache = vk_meta_lookup_pipeline(&device->meta_state.device, &key, sizeof(key));
   if (pipeline_from_cache != VK_NULL_HANDLE) {
      *pipeline_out = pipeline_from_cache;
      return VK_SUCCESS;
   }

   nir_shader *cs = build_depth_stencil_resolve_compute_shader(device, samples, index, resolve_mode);

   const VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = vk_shader_module_handle_from_nir(cs),
      .pName = "main",
      .pSpecializationInfo = NULL,
   };

   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage_info,
      .flags = 0,
      .layout = *layout_out,
   };

   result = vk_meta_create_compute_pipeline(&device->vk, &device->meta_state.device, &pipeline_info, &key, sizeof(key),
                                            pipeline_out);

   ralloc_free(cs);
   return result;
}

static void
emit_depth_stencil_resolve(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *src_iview,
                           struct radv_image_view *dst_iview, const VkOffset2D *resolve_offset,
                           const VkExtent3D *resolve_extent, VkImageAspectFlags aspects,
                           VkResolveModeFlagBits resolve_mode)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const uint32_t samples = src_iview->image->vk.samples;
   VkPipelineLayout layout;
   VkPipeline pipeline;
   VkResult result;

   result = get_depth_stencil_resolve_pipeline(device, samples, aspects, resolve_mode, &pipeline, &layout);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2,
                                 (VkWriteDescriptorSet[]){{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstBinding = 0,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                           .pImageInfo =
                                                              (VkDescriptorImageInfo[]){
                                                                 {.sampler = VK_NULL_HANDLE,
                                                                  .imageView = radv_image_view_to_handle(src_iview),
                                                                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
                                                              }},
                                                          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstBinding = 1,
                                                           .dstArrayElement = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                           .pImageInfo = (VkDescriptorImageInfo[]){
                                                              {
                                                                 .sampler = VK_NULL_HANDLE,
                                                                 .imageView = radv_image_view_to_handle(dst_iview),
                                                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                              },
                                                           }}});

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   uint32_t push_constants[2] = {resolve_offset->x, resolve_offset->y};

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(push_constants), push_constants);

   radv_unaligned_dispatch(cmd_buffer, resolve_extent->width, resolve_extent->height, resolve_extent->depth);
}

void
radv_meta_resolve_compute_image(struct radv_cmd_buffer *cmd_buffer, struct radv_image *src_image, VkFormat src_format,
                                VkImageLayout src_image_layout, struct radv_image *dst_image, VkFormat dst_format,
                                VkImageLayout dst_image_layout, const VkImageResolve2 *region)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_saved_state saved_state;

   /* For partial resolves, DCC should be decompressed before resolving
    * because the metadata is re-initialized to the uncompressed after.
    */
   uint32_t queue_mask = radv_image_queue_family_mask(dst_image, cmd_buffer->qf, cmd_buffer->qf);

   if (!radv_image_use_dcc_image_stores(device, dst_image) &&
       radv_layout_dcc_compressed(device, dst_image, region->dstSubresource.mipLevel, dst_image_layout, queue_mask) &&
       (region->dstOffset.x || region->dstOffset.y || region->dstOffset.z ||
        region->extent.width != dst_image->vk.extent.width || region->extent.height != dst_image->vk.extent.height ||
        region->extent.depth != dst_image->vk.extent.depth)) {
      radv_decompress_dcc(cmd_buffer, dst_image,
                          &(VkImageSubresourceRange){
                             .aspectMask = region->dstSubresource.aspectMask,
                             .baseMipLevel = region->dstSubresource.mipLevel,
                             .levelCount = 1,
                             .baseArrayLayer = region->dstSubresource.baseArrayLayer,
                             .layerCount = vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource),
                          });
   }

   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS);

   assert(region->srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
   assert(region->dstSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
   assert(vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource) ==
          vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource));

   const uint32_t dst_base_layer = radv_meta_get_iview_layer(dst_image, &region->dstSubresource, &region->dstOffset);

   const struct VkExtent3D extent = vk_image_sanitize_extent(&src_image->vk, region->extent);
   const struct VkOffset3D srcOffset = vk_image_sanitize_offset(&src_image->vk, region->srcOffset);
   const struct VkOffset3D dstOffset = vk_image_sanitize_offset(&dst_image->vk, region->dstOffset);
   const unsigned src_layer_count = vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);

   for (uint32_t layer = 0; layer < src_layer_count; ++layer) {

      struct radv_image_view src_iview;
      radv_image_view_init(&src_iview, device,
                           &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = radv_image_to_handle(src_image),
                              .viewType = VK_IMAGE_VIEW_TYPE_2D,
                              .format = src_format,
                              .subresourceRange =
                                 {
                                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .baseMipLevel = 0,
                                    .levelCount = 1,
                                    .baseArrayLayer = region->srcSubresource.baseArrayLayer + layer,
                                    .layerCount = 1,
                                 },
                           },
                           NULL);

      struct radv_image_view dst_iview;
      radv_image_view_init(&dst_iview, device,
                           &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = radv_image_to_handle(dst_image),
                              .viewType = radv_meta_get_view_type(dst_image),
                              .format = vk_format_no_srgb(dst_format),
                              .subresourceRange =
                                 {
                                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .baseMipLevel = region->dstSubresource.mipLevel,
                                    .levelCount = 1,
                                    .baseArrayLayer = dst_base_layer + layer,
                                    .layerCount = 1,
                                 },
                           },
                           NULL);

      emit_resolve(cmd_buffer, &src_iview, &dst_iview, &(VkOffset2D){srcOffset.x, srcOffset.y},
                   &(VkOffset2D){dstOffset.x, dstOffset.y}, &(VkExtent2D){extent.width, extent.height});

      radv_image_view_finish(&src_iview);
      radv_image_view_finish(&dst_iview);
   }

   radv_meta_restore(&saved_state, cmd_buffer);

   if (!radv_image_use_dcc_image_stores(device, dst_image) &&
       radv_layout_dcc_compressed(device, dst_image, region->dstSubresource.mipLevel, dst_image_layout, queue_mask)) {

      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE;

      VkImageSubresourceRange range = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = region->dstSubresource.mipLevel,
         .levelCount = 1,
         .baseArrayLayer = dst_base_layer,
         .layerCount = vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource),
      };

      cmd_buffer->state.flush_bits |= radv_init_dcc(cmd_buffer, dst_image, &range, 0xffffffff);
   }
}

void
radv_cmd_buffer_resolve_rendering_cs(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *src_iview,
                                     VkImageLayout src_layout, struct radv_image_view *dst_iview,
                                     VkImageLayout dst_layout, const VkImageResolve2 *region)
{
   radv_meta_resolve_compute_image(cmd_buffer, src_iview->image, src_iview->vk.format, src_layout, dst_iview->image,
                                   dst_iview->vk.format, dst_layout, region);

   cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE |
                                   radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                         VK_ACCESS_2_SHADER_WRITE_BIT, 0, NULL, NULL);
}

void
radv_depth_stencil_resolve_rendering_cs(struct radv_cmd_buffer *cmd_buffer, VkImageAspectFlags aspects,
                                        VkResolveModeFlagBits resolve_mode)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   VkRect2D resolve_area = render->area;
   struct radv_meta_saved_state saved_state;

   uint32_t layer_count = render->layer_count;
   if (render->view_mask)
      layer_count = util_last_bit(render->view_mask);

   /* Resolves happen before the end-of-subpass barriers get executed, so
    * we have to make the attachment shader-readable.
    */
   cmd_buffer->state.flush_bits |=
      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, NULL, NULL) |
      radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, 0, NULL,
                            NULL);

   struct radv_image_view *src_iview = render->ds_att.iview;
   VkImageLayout src_layout =
      aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? render->ds_att.layout : render->ds_att.stencil_layout;
   struct radv_image *src_image = src_iview->image;

   VkImageResolve2 region = {0};
   region.sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2;
   region.srcSubresource.aspectMask = aspects;
   region.srcSubresource.mipLevel = 0;
   region.srcSubresource.baseArrayLayer = src_iview->vk.base_array_layer;
   region.srcSubresource.layerCount = layer_count;

   radv_decompress_resolve_src(cmd_buffer, src_image, src_layout, &region);

   radv_meta_save(&saved_state, cmd_buffer, RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS);

   struct radv_image_view *dst_iview = render->ds_att.resolve_iview;
   VkImageLayout dst_layout =
      aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? render->ds_att.resolve_layout : render->ds_att.stencil_resolve_layout;
   struct radv_image *dst_image = dst_iview->image;

   struct radv_image_view tsrc_iview;
   radv_image_view_init(&tsrc_iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .image = radv_image_to_handle(src_image),
                           .viewType = VK_IMAGE_VIEW_TYPE_2D,
                           .format = src_iview->vk.format,
                           .subresourceRange =
                              {
                                 .aspectMask = aspects,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = src_iview->vk.base_array_layer,
                                 .layerCount = layer_count,
                              },
                        },
                        NULL);

   struct radv_image_view tdst_iview;
   radv_image_view_init(&tdst_iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .image = radv_image_to_handle(dst_image),
                           .viewType = radv_meta_get_view_type(dst_image),
                           .format = dst_iview->vk.format,
                           .subresourceRange =
                              {
                                 .aspectMask = aspects,
                                 .baseMipLevel = dst_iview->vk.base_mip_level,
                                 .levelCount = 1,
                                 .baseArrayLayer = dst_iview->vk.base_array_layer,
                                 .layerCount = layer_count,
                              },
                        },
                        NULL);

   emit_depth_stencil_resolve(cmd_buffer, &tsrc_iview, &tdst_iview, &resolve_area.offset,
                              &(VkExtent3D){resolve_area.extent.width, resolve_area.extent.height, layer_count},
                              aspects, resolve_mode);

   cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE |
                                   radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                         VK_ACCESS_2_SHADER_WRITE_BIT, 0, NULL, NULL);

   uint32_t queue_mask = radv_image_queue_family_mask(dst_image, cmd_buffer->qf, cmd_buffer->qf);

   if (radv_layout_is_htile_compressed(device, dst_image, dst_layout, queue_mask)) {
      VkImageSubresourceRange range = {0};
      range.aspectMask = aspects;
      range.baseMipLevel = dst_iview->vk.base_mip_level;
      range.levelCount = 1;
      range.baseArrayLayer = dst_iview->vk.base_array_layer;
      range.layerCount = layer_count;

      uint32_t htile_value = radv_get_htile_initial_value(device, dst_image);

      cmd_buffer->state.flush_bits |= radv_clear_htile(cmd_buffer, dst_image, &range, htile_value, false);
   }

   radv_image_view_finish(&tsrc_iview);
   radv_image_view_finish(&tdst_iview);

   radv_meta_restore(&saved_state, cmd_buffer);
}

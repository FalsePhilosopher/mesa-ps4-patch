/* These are sorted by the frequency of their occurrence for instruction cache
 * hits between case statements in the batch_execute switch. The benchmark is
 * VP2020/Catia1.
 *
 * This proves that such an optimization is potentially beneficial, though it's
 * very close to being noise. The CPU time spent in tc_batch_execute is reduced
 * by 0.8% in sysprof (not 0.8% from the original measured number, but 0.8%
 * from the original measured number scaled to 100%). 99% of TC calls
 * in the benchmark are just alternating between the first two.
 */
CALL(draw_single)
CALL(set_vertex_buffers)
CALL(set_constant_buffer)
CALL(bind_rasterizer_state)
CALL(buffer_unmap)
CALL(bind_vertex_elements_state)
CALL(bind_vs_state)
CALL(bind_fs_state)
CALL(bind_depth_stencil_alpha_state)
CALL(set_sampler_views)
CALL(set_viewport_states)
CALL(flush)
CALL(flush_resource)
CALL(resource_copy_region)
CALL(bind_sampler_states)
CALL(texture_unmap)
CALL(bind_blend_state)
CALL(draw_multi)
CALL(set_framebuffer_state)
CALL(clear)
CALL(set_shader_images)
CALL(set_shader_buffers)
CALL(delete_rasterizer_state)
CALL(delete_vs_state)
CALL(delete_vertex_elements_state)
CALL(delete_fs_state)
CALL(delete_depth_stencil_alpha_state)
CALL(set_min_samples)
CALL(delete_blend_state)
CALL(set_stream_output_targets)
CALL(set_stencil_ref)
CALL(set_sample_mask)
CALL(bind_tes_state)
CALL(bind_tcs_state)
CALL(bind_gs_state)
CALL(bind_compute_state)
CALL(set_tess_state)
CALL(set_polygon_stipple)
CALL(set_patch_vertices)
CALL(set_blend_color)
CALL(replace_buffer_storage)
CALL(delete_sampler_state)

CALL(flush_deferred)
CALL(callback)
CALL(fence_server_sync)
CALL(destroy_query)
CALL(begin_query)
CALL(end_query)
CALL(get_query_result_resource)
CALL(render_condition)
CALL(set_inlinable_constants)
CALL(set_sample_locations)
CALL(set_scissor_states)
CALL(set_window_rectangles)
CALL(transfer_flush_region)
CALL(buffer_subdata)
CALL(texture_subdata)
CALL(emit_string_marker)
CALL(draw_single_drawid)
CALL(draw_indirect)
CALL(draw_vstate_single)
CALL(draw_vstate_multi)
CALL(launch_grid)
CALL(blit)
CALL(generate_mipmap)
CALL(invalidate_resource)
CALL(clear_render_target)
CALL(clear_depth_stencil)
CALL(clear_buffer)
CALL(clear_texture)
CALL(resource_commit)
CALL(set_active_query_state)
CALL(set_clip_state)
CALL(texture_barrier)
CALL(memory_barrier)
CALL(delete_texture_handle)
CALL(make_texture_handle_resident)
CALL(delete_image_handle)
CALL(make_image_handle_resident)
CALL(set_context_param)
CALL(set_frontend_noop)

CALL(delete_compute_state)
CALL(delete_gs_state)
CALL(delete_tcs_state)
CALL(delete_tes_state)

CALL(begin_intel_perf_query)
CALL(end_intel_perf_query)

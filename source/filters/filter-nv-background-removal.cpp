/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2017 Michael Fabian Dirks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "filter-nv-background-removal.hpp"
#include <algorithm>
#include <filesystem>
#include <util/platform.h>

#include "nvidia/cuda/nvidia-cuda-context.hpp"
#include "obs/gs/gs-helper.hpp"
#include "obs/obs-tools.hpp"
#include "util/util-logging.hpp"

#define ST "Filter.Nvidia.BackgroundRemoval"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<filter::nv_background_removal> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

using namespace streamfx::filter::nvidia;

static constexpr std::string_view HELP_URL = "https://github.com/Xaymar/obs-StreamFX/wiki/Background-Removal";

//------------------------------------------------------------------------------
// Instance
//------------------------------------------------------------------------------

background_removal_instance::background_removal_instance(obs_data_t* settings, obs_source_t* self)
	: obs::source_instance(settings, self), _rt_is_fresh(false), _rt(), _nvcuda(::nvidia::cuda::obs::get()),
	  _nvcvi(::nvidia::cv::cv::get()), _nvvfx(::nvidia::vfx::vfx::get()), _cuda_stream()
{
#ifdef ENABLE_PROFILING
	// Profiling
	_profile_capture         = util::profiler::create();
	_profile_capture_realloc = util::profiler::create();
	_profile_capture_copy    = util::profiler::create();
	_profile_ar_realloc      = util::profiler::create();
	_profile_ar_copy         = util::profiler::create();
	_profile_ar_transfer     = util::profiler::create();
	_profile_ar_run          = util::profiler::create();
	_profile_ar_calc         = util::profiler::create();
#endif

	{ // Create render target, and CUDA stream.
		gs::context gctx;
		// Create the render target for the input buffering.
		_input = std::make_shared<gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_input->render(1, 1); // Preallocate the RT on the driver and GPU.

		auto cctx    = _nvcuda->get_context()->enter();
		_cuda_stream = std::make_shared<::nvidia::cuda::stream>(::nvidia::cuda::stream_flags::DEFAULT, 0);
	}

	{
		auto result = _nvvfx->NvVFX_CreateEffect(::nvidia::vfx::EFFECT_GREEN_SCREEN, &_nv_effect); //create the effect
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_CreateEffect with error: %d", result);
		}
	}
}

background_removal_instance::~background_removal_instance()
{
	{
		auto result = _nvvfx->NvVFX_DestroyEffect(&_nv_effect);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_DestroyEffect with error: %d", result);
		}
	}

	{ // Clean up any GPU resources in use.
		gs::context gctx = {};
		auto        cctx = _nvcuda->get_context()->enter();

		// Clean up any CUDA resources in use.
		if (_nvidia_cvi_input.width != 0) {
			if (auto res = _nvcvi->NvCVImage_UnmapResource(&_nvidia_cvi_input, _nvcuda->get_stream()->get());
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_UnmapResource input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				//throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_input); res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				//throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_working_input);
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				//throw std::runtime_error("Error");
			}
		}
		if (_nvidia_cvi_output.width != 0) {
			if (auto res = _nvcvi->NvCVImage_UnmapResource(&_nvidia_cvi_output, _nvcuda->get_stream()->get());
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_UnmapResource output with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				//throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_output); res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc output with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				//throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_working_output);
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				//throw std::runtime_error("Error");
			}
		}
	}
}

uint32_t background_removal_instance::get_width()
{
	return _size.first;
}

uint32_t background_removal_instance::get_height()
{
	return _size.second;
}

void background_removal_instance::video_tick(float_t time) {}

void background_removal_instance::video_render(gs_effect_t* effect)
{
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	vec4 blank  = vec4{0, 0, 0, 0};

	// Ensure we have the bare minimum of valid information.
	target = target ? target : obs_filter_get_parent(_self);
	effect = effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);

	// Skip the filter if:
	// - The Provider isn't ready yet.
	// - The width/height of the next filter in the chain is empty.
	// - We don't have a target.
	if (!target || (width == 0) || (height == 0)) {
		obs_source_skip_video_filter(_self);
		return;
	}

	std::shared_ptr<gs::texture> _input_texture;

	{
		_size = _enforce_size_nvidia_background_removal(width, height);

		// Capture the input.

		if (obs_source_process_filter_begin(_self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
			auto op = _input->render(width, height);

			// Clear the buffer
			gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &blank, 0, 0);

			// Set GPU state
			gs_blend_state_push();
			gs_enable_color(true, true, true, true);
			gs_enable_blending(false);
			gs_enable_depth_test(false);
			gs_enable_stencil_test(false);
			gs_set_cull_mode(GS_NEITHER);

			// Render
			//bool srgb = gs_framebuffer_srgb_enabled();
			//gs_enable_framebuffer_srgb(gs_get_linear_srgb());
			//if (gs_get_linear_srgb()) {
			//	obs_source_process_filter_end_srgb(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
			//} else {
			obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
			//}
			//gs_enable_framebuffer_srgb(srgb);

			_input->get_texture(_input_texture);

			{
				auto cctx = _nvcuda->get_context()->enter();
				//auto texture = _nvidia_input->get_texture();

				ensure_nvidia_rt(_nvidia_input, _nvidia_cvi_input, _nvidia_cvi_working_input);
				ensure_nvidia_rt(_nvidia_output, _nvidia_cvi_output, _nvidia_cvi_working_output);

				gs_copy_texture(_nvidia_input->get_object(), _input->get_object());
			}

			// Reset GPU state
			gs_blend_state_pop();
		} else {
			obs_source_skip_video_filter(_self);
			return;
		}

		// Process the captured input with the provider.
		if (_input_texture) {
			//Do render now.
			//process_nvidia_background_removal(_input_texture.get());
		} else {
			obs_source_skip_video_filter(_self);
			return;
		}
	}

	{ // Draw the result for the next filter to use.
		// Revert GPU status to what OBS Studio expects.
		gs_enable_depth_test(false);
		gs_enable_color(true, true, true, true);
		gs_set_cull_mode(GS_NEITHER);

		// Draw the render cache.
		while (gs_effect_loop(effect, "Draw")) {
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"),
								  _nvidia_output ? _nvidia_output->get_object() : nullptr);
			gs_draw_sprite(nullptr, 0, width, height);
		}
	}
}

void background_removal_instance::ensure_nvidia_rt(std::shared_ptr<gs::texture>& texture, ::nvidia::cv::image_t& nvimg,
												   ::nvidia::cv::image_t& nvimg_working)
{
	if (texture && (texture->get_width() == _size.first) && (texture->get_height() == _size.second)) {
		return;
	}

	// Unmap and deallocate previous resource.
	if (nvimg.width != 0) {
		if (auto res = _nvcvi->NvCVImage_UnmapResource(&nvimg, _nvcuda->get_stream()->get());
			res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_UnmapResource input with error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
		if (auto res = _nvcvi->NvCVImage_Dealloc(&nvimg); res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
		if (auto res = _nvcvi->NvCVImage_Dealloc(&nvimg_working); res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
	}

	texture =
		std::make_shared<gs::texture>(_size.first, _size.second, GS_RGBA_UNORM, 1, nullptr, gs::texture::flags::None);

	// Allocate and map new resource.
	if (auto res = _nvcvi->NvCVImage_InitFromD3D11Texture(
			&nvimg, reinterpret_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture->get_object())));
		res != ::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to NvCVImage_InitFromD3D11Texture input with error: %s",
					_nvcvi->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("Error");
	}
	if (auto res = _nvcvi->NvCVImage_MapResource(&nvimg, _nvcuda->get_stream()->get());
		res != ::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to NvCVImage_MapResource input with error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("Error");
	}

	if (auto res = _nvcvi->NvCVImage_Alloc(&nvimg_working, _size.first, _size.second, ::nvidia::cv::pixel_format::RGBA,
										   ::nvidia::cv::component_type::UINT8, ::nvidia::cv::component_layout::PLANAR,
										   ::nvidia::cv::memory_location::GPU, 0);
		res != ::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to NvCVImage_Alloc input with error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("Error");
	}
}

std::shared_ptr<gs::texture> background_removal_instance::process_nvidia_background_removal()
{
	auto cctx = _nvcuda->get_context()->enter();

	_nvcuda->get_context()->synchronize();
	_cuda_stream->synchronize();

	{
		auto result =
			_nvcvi->NvCVImage_Transfer(&_nvidia_cvi_input, &_nvidia_cvi_working_input, 1, _cuda_stream->get(), nullptr);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvCVImage_Transfer on INPUT with error: %d", result);
		}
	}

	{
		auto result = _nvcvi->NvCVImage_Transfer(&_nvidia_cvi_working_input, &_nvidia_cvi_working_output, 1,
												 _cuda_stream->get(), nullptr);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvCVImage_Transfer on INPUT with error: %d", result);
		}
	}

	/*{
		auto result =
			_nvvfx->NvVFX_SetImage(_nv_effect, ::nvidia::vfx::PARAMETER_INPUT_IMAGE_0, &_nvidia_cvi_working_input);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_SetImage on INPUT with error: %d", result);
		}
	}
	{
		auto result =
			_nvvfx->NvVFX_SetImage(_nv_effect, ::nvidia::vfx::PARAMETER_OUTPUT_IMAGE_0, &_nvidia_cvi_working_output);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_SetImage on OUTPUT with error: %d", result);
		}
	}
	{
		auto result =
			_nvvfx->NvVFX_SetCudaStream(_nv_effect, ::nvidia::vfx::PARAMETER_CUDA_STREAM, _cuda_stream->get());
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_SetCudaStream with error: %d", result);
		}
	}
	{
		auto result = _nvvfx->NvVFX_Load(_nv_effect);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_Load with error: %d", result);
		}
	}
	{
		auto result = _nvvfx->NvVFX_Run(_nv_effect, 0);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvVFX_Run with error: %d", result);
		}
	}*/

	{
		auto result = _nvcvi->NvCVImage_Transfer(&_nvidia_cvi_working_output, &_nvidia_cvi_output, 1,
												 _cuda_stream->get(), nullptr);
		if (result != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed NvCVImage_Transfer on INPUT with error: %d", result);
		}
	}

	_nvcuda->get_context()->synchronize();
	_cuda_stream->synchronize();

	return _nvidia_output;
}

std::pair<uint32_t, uint32_t> background_removal_instance::_enforce_size_nvidia_background_removal(uint32_t x,
																								   uint32_t y)
{
	// We need this???
	// NVIDIA Video Noise Removal documentation only states a vertical limit of
	// minimum 80p and maximum 1080p, with no hints on horizontal limits. It is
	// assumed that there are limits on both, as 80p/1080p is often used for 16:9
	// resolutions.

	if (x > y) {
		// Dominant Width
		double   ar = static_cast<double>(y) / static_cast<double>(x);
		uint32_t rx = std::clamp<uint32_t>(x, 142, 1920); // 80p - 1080p
		uint32_t ry = static_cast<uint32_t>(round(static_cast<double>(rx) * ar));
		return {rx, ry};
	} else {
		// Dominant Height
		double   ar = static_cast<double>(x) / static_cast<double>(y);
		uint32_t ry = std::clamp<uint32_t>(y, 80, 1080); // 80p - 1080p
		uint32_t rx = static_cast<uint32_t>(round(static_cast<double>(ry) * ar));
		return {rx, ry};
	}
}

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------
background_removal_factory::~background_removal_factory() {}

background_removal_factory::background_removal_factory()
{
	bool any_available = false;

	// 1. Try and load any configured providers.
	try {
		// Load CVImage and Video Effects SDK.
		_nvcuda           = ::nvidia::cuda::obs::get();
		_nvcvi            = ::nvidia::cv::cv::get();
		_nvvfx            = ::nvidia::vfx::vfx::get();
		_nvidia_available = true;
		any_available |= _nvidia_available;
	} catch (const std::exception& ex) {
		_nvidia_available = false;
		_nvvfx.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA Video Effects background removal available due to error: %s", ex.what());
	}

	// 2. Check if any of them managed to load at all.
	if (!any_available) {
		D_LOG_ERROR("All supported denoising providers failed to initialize, disabling effect.", 0);
		return;
	}

	// 3. In any other case, register the filter!
	_info.id           = PREFIX "filter-nvidia-background-removal";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO;

	set_resolution_enabled(true); //???
	finish_setup();
}

const char* background_removal_factory::get_name()
{
	return D_TRANSLATE(ST);
}

void background_removal_factory::get_defaults2(obs_data_t* data)
{
	//Set default settings for the filter???
}

obs_properties_t* background_removal_factory::get_properties2(background_removal_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

	{ // Advanced Settings
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, S_ADVANCED, D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);
	}

	return pr;
}

std::shared_ptr<background_removal_factory> _background_removal_factory_instance = nullptr;

void background_removal_factory::initialize()
{
	if (!_background_removal_factory_instance) {
		try {
			_background_removal_factory_instance = std::make_shared<background_removal_factory>();
		} catch (const std::exception& ex) {
			DLOG_ERROR("<NVIDIA Background Removal Filter> %s", ex.what());
		}
	}
}

void background_removal_factory::finalize()
{
	_background_removal_factory_instance.reset();
}

std::shared_ptr<background_removal_factory> background_removal_factory::get()
{
	return _background_removal_factory_instance;
}

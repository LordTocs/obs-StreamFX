// Copyright (c) 2020 Michael Fabian Dirks <info@xaymar.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "filter-video-denoising.hpp"
#include <algorithm>
#include "obs/gs/gs-helper.hpp"
#include "plugin.hpp"
#include "util/util-logging.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<filter::video_denoising> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

#define ST "Filter.VideoDenoising"
#define ST_PROVIDER ST ".Provider"
#define ST_PROVIDER_NVIDIA_VIDEO_NOISE_REMOVAL ST_PROVIDER ".NVIDIAVideoNoiseRemoval"

using streamfx::filter::video_denoising::denoise_provider;
using streamfx::filter::video_denoising::video_denoising_factory;
using streamfx::filter::video_denoising::video_denoising_instance;

static constexpr std::string_view HELP_URL = "https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Video-Denoising";

static denoise_provider provider_priority[] = {
	denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL,
};

const char* streamfx::filter::video_denoising::denoise_provider_to_string(denoise_provider provider)
{
	switch (provider) {
	case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
		return D_TRANSLATE(ST_PROVIDER_NVIDIA_VIDEO_NOISE_REMOVAL);
	default:
		throw std::runtime_error("Missing Conversion Entry");
	}
}

//------------------------------------------------------------------------------
// Instance
//------------------------------------------------------------------------------
video_denoising_instance::video_denoising_instance(obs_data_t* data, obs_source_t* self)
	: obs::source_instance(data, self),

	  _size(1, 1), _provider_ready(false), _provider(denoise_provider::AUTOMATIC), _provider_lock(), _provider_task(),
	  _input(), _output()
{
	{
		gs::context gctx;

		// Create the render target for the input buffering.
		_input = std::make_shared<gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_input->render(1, 1); // Preallocate the RT on the driver and GPU.
	}

	if (data) {
		load(data);
	}
}

video_denoising_instance::~video_denoising_instance()
{
	// TODO: Make this asynchronous.
	std::unique_lock<std::mutex> ul(_provider_lock);
	switch (_provider) {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
	case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
		unload_nvidia_noise_removal();
		break;
#endif
	default:
		break;
	}
}

void video_denoising_instance::load(obs_data_t* data)
{
	update(data);
}

void video_denoising_instance::migrate(obs_data_t* data, uint64_t version) {}

void video_denoising_instance::update(obs_data_t* data)
{
	// Check if the user changed which Denoising provider we use.
	denoise_provider provider = static_cast<denoise_provider>(obs_data_get_int(data, ST_PROVIDER));
	if (provider == denoise_provider::AUTOMATIC) {
		for (auto v : provider_priority) {
			if (video_denoising_factory::get()->is_provider_available(v)) {
				provider = v;
				break;
			}
		}
	}
	if (provider != _provider) {
		// The provider is different from the original, recreate the provider.
		switch_provider(provider);
	}
}

uint32_t streamfx::filter::video_denoising::video_denoising_instance::get_width()
{
	return _size.first;
}

uint32_t streamfx::filter::video_denoising::video_denoising_instance::get_height()
{
	return _size.second;
}

void video_denoising_instance::video_tick(float_t time) {}

void video_denoising_instance::video_render(gs_effect_t* effect)
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
	if (!_provider_ready || !target || (width == 0) || (height == 0)) {
		obs_source_skip_video_filter(_self);
		return;
	}

	{ // Lock the provider from being changed.
		std::unique_lock<std::mutex> ul(_provider_lock);

		{ // Allow the provider to restrict the size.
			switch (_provider) {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
			case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
				_size = resize_nvidia_noise_removal(width, height);
				break;
#endif
			default:
				_size = {width, height};
				break;
			}
		}

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
			bool srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(gs_get_linear_srgb());
			//if (gs_get_linear_srgb()) {
			//	obs_source_process_filter_end_srgb(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
			//} else {
			obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
			//}
			gs_enable_framebuffer_srgb(srgb);

			// Reset GPU state
			gs_blend_state_pop();
		} else {
			obs_source_skip_video_filter(_self);
			return;
		}

		// Process the captured input with the provider.
		{
			switch (_provider) {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
			case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
				_output = process_nvidia_noise_removal();
				break;
#endif
			default:
				_output = nullptr;
				break;
			}

			if (!_output) {
				D_LOG_ERROR("Provider '%s' did not return a result.", denoise_provider_to_string(_provider));
				obs_source_skip_video_filter(_self);
				return;
			}
		}

		// Unlock the provider, as we are no longer doing critical work with it.
	}

	{ // Throw away the input buffer (no longer needed).
		auto op = _input->render(1, 1);
	}

	{ // Draw the result for the next filter to use.
		if (gs_get_linear_srgb()) {
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), _output->get_object());
		} else {
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), _output->get_object());
		}
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(nullptr, 0, _size.first, _size.second);
		}
	}
}

struct switch_provider_data_t {
	denoise_provider provider;
};

void streamfx::filter::video_denoising::video_denoising_instance::switch_provider(denoise_provider provider)
{
	std::unique_lock<std::mutex> ul(_provider_lock);

	// This won't work correctly.
	// - Need to allow multiple switches at once because OBS is weird.
	// - Doesn't guarantee that the task is properly killed off.

	// 1.If there is an existing task, attempt to cancel it.
	if (_provider_task) {
		streamfx::threadpool()->pop(_provider_task);
	}

	// 2. Then spawn a new task to switch provider.
	std::shared_ptr<switch_provider_data_t> spd = std::make_shared<switch_provider_data_t>();
	spd->provider                               = provider;
	_provider_task                              = streamfx::threadpool()->push(
        std::bind(&video_denoising_instance::task_switch_provider, this, std::placeholders::_1), spd);
}

void streamfx::filter::video_denoising::video_denoising_instance::task_switch_provider(util::threadpool_data_t data)
{
	std::shared_ptr<switch_provider_data_t> spd = std::static_pointer_cast<switch_provider_data_t>(data);

	// 1. Mark the provider as no longer ready.
	_provider_ready = false;

	// 2. Lock the provider from being used.
	std::unique_lock<std::mutex> ul(_provider_lock);

	// 3. Load the new provider.
	switch (spd->provider) {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
	case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
		load_nvidia_noise_removal();
		break;
#endif
	default:
		break;
	}

	// 4. Unload the previous provider.
	switch (_provider) {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
	case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
		unload_nvidia_noise_removal();
		break;
#endif
	default:
		break;
	}

	// 5. Set the new provider as valid.
	_provider       = spd->provider;
	_provider_ready = true;
}

#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
void streamfx::filter::video_denoising::video_denoising_instance::load_nvidia_noise_removal()
{
	_nvcuda = ::nvidia::cuda::obs::get();
	_nvcvi  = ::nvidia::cv::cv::get();
	_nvvfx  = ::nvidia::vfx::vfx::get();

	// Need:
	// - Buffer for Input image (always given)
	// - CVImage mapped to input buffer. (dynamically created)
	// - Feature (can this be dynamically created?
	// - Buffer for output image. (dynamically created)
	// - CVImage mapped to output buffer. (dynamically created)
}

void streamfx::filter::video_denoising::video_denoising_instance::unload_nvidia_noise_removal()
{
	{ // Clean up any GPU resources in use.
		gs::context gctx = {};
		auto        cctx = _nvcuda->get_context()->enter();

		// Clean up any CUDA resources in use.
		if (_nvidia_cvi_input.width != 0) {
			if (auto res = _nvcvi->NvCVImage_UnmapResource(&_nvidia_cvi_input, _nvcuda->get_stream()->get());
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_UnmapResource input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_input); res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
		}
		if (_nvidia_cvi_output.width != 0) {
			if (auto res = _nvcvi->NvCVImage_UnmapResource(&_nvidia_cvi_output, _nvcuda->get_stream()->get());
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_UnmapResource output with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_output); res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc output with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
		}

		_nvidia_input.reset();
		_nvidia_output.reset();
	}

	_nvvfx.reset();
	_nvcvi.reset();
	_nvcuda.reset();
}

std::pair<uint32_t, uint32_t>
	streamfx::filter::video_denoising::video_denoising_instance::resize_nvidia_noise_removal(uint32_t x, uint32_t y)
{
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

std::shared_ptr<gs::texture> streamfx::filter::video_denoising::video_denoising_instance::process_nvidia_noise_removal()
{
	auto cctx    = _nvcuda->get_context()->enter();
	auto texture = _input->get_texture();

	// Re-create the input buffer if necessary.
	if (!_nvidia_input || (_nvidia_input->get_width() != _size.first)
		|| (_nvidia_input->get_height() != _size.second)) {
		// Unmap and deallocate previous resource.
		if (_nvidia_cvi_input.width != 0) {
			if (auto res = _nvcvi->NvCVImage_UnmapResource(&_nvidia_cvi_input, _nvcuda->get_stream()->get());
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_UnmapResource input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_input); res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc input with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
		}

		// Replace buffer texture
		_nvidia_input = std::make_shared<gs::texture>(_size.first, _size.second, GS_RGBA_UNORM, 1, nullptr,
													  gs::texture::flags::None);

		// Allocate and map new resource.
		if (auto res = _nvcvi->NvCVImage_InitFromD3D11Texture(
				&_nvidia_cvi_input,
				reinterpret_cast<ID3D11Texture2D*>(gs_texture_get_obj(_nvidia_input->get_object())));
			res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_InitFromD3D11Texture input with error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
		if (auto res = _nvcvi->NvCVImage_MapResource(&_nvidia_cvi_input, _nvcuda->get_stream()->get());
			res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_MapResource input with error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
	}

	// Re-create the output buffer if necessary.
	if (!_nvidia_output || (_nvidia_output->get_width() != _size.first)
		|| (_nvidia_output->get_height() != _size.second)) {
		// Unmap and deallocate previous resource.
		if (_nvidia_cvi_output.width != 0) {
			if (auto res = _nvcvi->NvCVImage_UnmapResource(&_nvidia_cvi_output, _nvcuda->get_stream()->get());
				res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_UnmapResource output with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
			if (auto res = _nvcvi->NvCVImage_Dealloc(&_nvidia_cvi_output); res != ::nvidia::cv::result::SUCCESS) {
				D_LOG_ERROR("Failed to NvCVImage_Dealloc output with error: %s",
							_nvcvi->NvCV_GetErrorStringFromCode(res));
				throw std::runtime_error("Error");
			}
		}

		// Replace buffer texture
		_nvidia_output = std::make_shared<gs::texture>(_size.first, _size.second, GS_RGBA_UNORM, 1, nullptr,
													   gs::texture::flags::None);

		// Allocate and map new resource.
		if (auto res = _nvcvi->NvCVImage_InitFromD3D11Texture(
				&_nvidia_cvi_output,
				reinterpret_cast<ID3D11Texture2D*>(gs_texture_get_obj(_nvidia_output->get_object())));
			res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_InitFromD3D11Texture output with error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
		if (auto res = _nvcvi->NvCVImage_MapResource(&_nvidia_cvi_output, _nvcuda->get_stream()->get());
			res != ::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to NvCVImage_MapResource output with error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Error");
		}
	}

	// Copy the input capture to the CVImage buffer.
	gs_copy_texture(_nvidia_input->get_object(), texture->get_object());

	return _nvidia_output;
}

#endif

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------
video_denoising_factory::~video_denoising_factory() {}

video_denoising_factory::video_denoising_factory()
{
	bool any_available = false;

	// 1. Try and load any configured providers.
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
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
		D_LOG_WARNING("Failed to make NVIDIA Video Effects denoising available due to error: %s", ex.what());
	}
#endif

	// 2. Check if any of them managed to load at all.
	if (!any_available) {
		D_LOG_ERROR("All supported denoising providers failed to initialize, disabling effect.", 0);
		return;
	}

	// 3. In any other case, register the filter!
	_info.id           = PREFIX "filter-video-denoising";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO;

	set_resolution_enabled(true);
	finish_setup();
}

const char* video_denoising_factory::get_name()
{
	return D_TRANSLATE(ST);
}

void video_denoising_factory::get_defaults2(obs_data_t* data) {}

obs_properties_t* video_denoising_factory::get_properties2(video_denoising_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

#ifdef ENABLE_FRONTEND
	{
		obs_properties_add_button2(pr, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN),
								   video_denoising_factory::on_manual_open, nullptr);
	}
#endif

	{ // Advanced Settings
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, S_ADVANCED, D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_PROVIDER, D_TRANSLATE(ST_PROVIDER), OBS_COMBO_TYPE_LIST,
											 OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(p, D_TRANSLATE(S_STATE_AUTOMATIC),
									  static_cast<int64_t>(denoise_provider::AUTOMATIC));
			obs_property_list_add_int(p, D_TRANSLATE(ST_PROVIDER_NVIDIA_VIDEO_NOISE_REMOVAL),
									  static_cast<int64_t>(denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL));
		}
	}

	return pr;
}

#ifdef ENABLE_FRONTEND
bool video_denoising_factory::on_manual_open(obs_properties_t* props, obs_property_t* property, void* data)
{
	streamfx::open_url(HELP_URL);
	return false;
}
#endif

bool streamfx::filter::video_denoising::video_denoising_factory::is_provider_available(denoise_provider provider)
{
	switch (provider) {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
	case denoise_provider::NVIDIA_VIDEO_NOISE_REMOVAL:
		return _nvidia_available;
#endif
	default:
		return false;
	}
}

std::shared_ptr<video_denoising_factory> _video_denoising_factory_instance = nullptr;

void video_denoising_factory::initialize()
{
	if (!_video_denoising_factory_instance)
		_video_denoising_factory_instance = std::make_shared<video_denoising_factory>();
}

void video_denoising_factory::finalize()
{
	_video_denoising_factory_instance.reset();
}

std::shared_ptr<video_denoising_factory> video_denoising_factory::get()
{
	return _video_denoising_factory_instance;
}

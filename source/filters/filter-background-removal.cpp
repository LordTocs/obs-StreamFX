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

#include "filter-background-removal.hpp"
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
#define ST_PREFIX "<filter::background_removal> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

#define ST_I18N "Filter.BackgroundRemoval"
#define ST_KEY_PROVIDER "Provider"
#define ST_I18N_PROVIDER ST_I18N "." ST_KEY_PROVIDER
#define ST_I18N_PROVIDER_NVIDIA_BGRMVL ST_I18N_PROVIDER ".NVIDIAVideoBackgroundRemoval"

#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
#define ST_KEY_NVIDIA_GREEN_SCREEN "NVIDIA.GreenScreen"
#define ST_I18N_PROVIDER_NVIDIA_GREEN_SCREEN ST_I18N "." ST_KEY_NVIDIA_GREEN_SCREEN
#endif

using streamfx::filter::background_removal::background_removal_factory;
using streamfx::filter::background_removal::background_removal_instance;
using streamfx::filter::background_removal::background_removal_provider;

static constexpr std::string_view HELP_URL =
	"https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Video-Super-Resolution";

/** Priority of providers for automatic selection if more than one is available.
 * 
 */
static background_removal_provider provider_priority[] = {
	background_removal_provider::NVIDIA_GREEN_SCREEN,
};

const char* streamfx::filter::background_removal::cstring(background_removal_provider provider)
{
	switch (provider) {
	case background_removal_provider::INVALID:
		return "N/A";
	case background_removal_provider::AUTOMATIC:
		return D_TRANSLATE(S_STATE_AUTOMATIC);
	case background_removal_provider::NVIDIA_GREEN_SCREEN:
		return D_TRANSLATE(ST_KEY_NVIDIA_GREEN_SCREEN);
	default:
		throw std::runtime_error("Missing Conversion Entry");
	}
}

std::string streamfx::filter::background_removal::string(background_removal_provider provider)
{
	return cstring(provider);
}

//------------------------------------------------------------------------------
// Instance
//------------------------------------------------------------------------------
background_removal_instance::background_removal_instance(obs_data_t* data, obs_source_t* self)
	: obs::source_instance(data, self),

	  _in_size(1, 1), _out_size(1, 1), _provider_ready(false), _provider(background_removal_provider::INVALID),
	  _provider_lock(), _provider_task(), _input(), _mask(), _dirty(false)
{
	{
		::streamfx::obs::gs::context gctx;

		// Create the render target for the input buffering.
		_input = std::make_shared<::streamfx::obs::gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_input->render(1, 1); // Preallocate the RT on the driver and GPU.

		_masked = std::make_shared<::streamfx::obs::gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_masked->render(1, 1); // Preallocate the RT on the driver and GPU.

		_mask = _input->get_texture();
	}

	try {
		_effect = streamfx::obs::gs::effect::create(streamfx::data_file_path("effects/channel-mask.effect").u8string());
	} catch (const std::exception& ex) {
		DLOG_ERROR("Loading channel mask effect failed with error(s):\n%s", ex.what());
	}

	if (data) {
		load(data);
	}
}

background_removal_instance::~background_removal_instance()
{
	// TODO: Make this asynchronous.
	std::unique_lock<std::mutex> ul(_provider_lock);
	switch (_provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
	case background_removal_provider::NVIDIA_GREEN_SCREEN:
		nvidia_unload();
		break;
#endif
	default:
		break;
	}
}

void background_removal_instance::load(obs_data_t* data)
{
	update(data);
}

void background_removal_instance::migrate(obs_data_t* data, uint64_t version) {}

void background_removal_instance::update(obs_data_t* data)
{
	// Check if the user changed which Denoising provider we use.
	background_removal_provider provider =
		static_cast<background_removal_provider>(obs_data_get_int(data, ST_KEY_PROVIDER));
	if (provider == background_removal_provider::AUTOMATIC) {
		for (auto v : provider_priority) {
			if (background_removal_factory::get()->is_provider_available(v)) {
				provider = v;
				break;
			}
		}
	}

	// Check if the provider was changed, and if so switch.
	if (provider != _provider) {
		switch_provider(provider);
	}

	if (_provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		case background_removal_provider::NVIDIA_GREEN_SCREEN:
			//nvidia_update(data);
			break;
#endif
		default:
			break;
		}
	}
}

void streamfx::filter::background_removal::background_removal_instance::properties(obs_properties_t* properties)
{
	if (_provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		case background_removal_provider::NVIDIA_GREEN_SCREEN:
			//nvidia_properties(properties);
			break;
#endif
		default:
			break;
		}
	}
}

uint32_t streamfx::filter::background_removal::background_removal_instance::get_width()
{
	return std::max<uint32_t>(_out_size.first, 1);
}

uint32_t streamfx::filter::background_removal::background_removal_instance::get_height()
{
	return std::max<uint32_t>(_out_size.second, 1);
}

void background_removal_instance::video_tick(float_t time)
{
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	_in_size    = {width, height};
	_out_size   = _in_size;

	// Allow the provider to restrict the size.
	if (target && _provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		case background_removal_provider::NVIDIA_GREEN_SCREEN:
			nvidia_size();
			break;
#endif
		default:
			break;
		}
	}

	_dirty = true;
}

void background_removal_instance::video_render(gs_effect_t* effect)
{
	auto parent = obs_filter_get_parent(_self);
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	vec4 blank  = vec4{0, 0, 0, 0};

	// Ensure we have the bare minimum of valid information.
	target = target ? target : parent;
	effect = effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);

	// Skip the filter if:
	// - The Provider isn't ready yet.
	// - We don't have a target.
	// - The width/height of the next filter in the chain is empty.
	if (!_provider_ready || !target || (width == 0) || (height == 0)) {
		obs_source_skip_video_filter(_self);
		return;
	}

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler0{::streamfx::obs::gs::debug_color_source, "StreamFX Background Removal"};
	::streamfx::obs::gs::debug_marker profiler0_0{::streamfx::obs::gs::debug_color_gray, "'%s' on '%s'",
												  obs_source_get_name(_self), obs_source_get_name(parent)};
#endif

	if (_dirty) {
		// Lock the provider from being changed.
		std::unique_lock<std::mutex> ul(_provider_lock);

		{ // Capture the incoming frame.
#ifdef ENABLE_PROFILING
			::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_capture, "Capture"};
#endif
			if (obs_source_process_filter_begin(_self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
				auto op = _input->render(_in_size.first, _in_size.second);

				// Matrix
				gs_matrix_push();
				gs_ortho(0., 1., 0., 1., 0., 1.);

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
#ifdef ENABLE_PROFILING
				::streamfx::obs::gs::debug_marker profiler2{::streamfx::obs::gs::debug_color_capture, "Storage"};
#endif
				obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), 1, 1);

				// Reset GPU state
				gs_blend_state_pop();
				gs_matrix_pop();
			} else {
				obs_source_skip_video_filter(_self);
				return;
			}
		}

		try { // Process the captured input with the provider.
#ifdef ENABLE_PROFILING
			::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_convert, "Process"};
#endif
			switch (_provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
			case background_removal_provider::NVIDIA_GREEN_SCREEN:
				nvidia_process();
				break;
#endif
			default:
				_mask.reset();
				break;
			}
		} catch (...) {
			obs_source_skip_video_filter(_self);
			return;
		}

		if (!_mask) {
			D_LOG_ERROR("Provider '%s' did not return a result.", cstring(_provider));
			obs_source_skip_video_filter(_self);
			return;
		}

		//Mask the input with the mask from the provider.
		try {
			auto op = _masked->render(width, height);

			gs_blend_state_push();
			gs_reset_blend_state();
			gs_enable_blending(false);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			gs_set_cull_mode(GS_NEITHER);
			gs_enable_color(true, true, true, true);

			gs_enable_depth_test(false);
			gs_depth_function(GS_ALWAYS);

			gs_enable_stencil_test(false);
			gs_enable_stencil_write(false);
			gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
			gs_stencil_op(GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_KEEP);
			gs_ortho(0, 1, 0, 1, -1., 1.);

			_effect.get_parameter("pMaskInputA").set_texture(_input->get_texture());
			_effect.get_parameter("pMaskInputB").set_texture(_mask);

			vec4 base;
			vec4_set(&base, 1.0f, 1.0f, 1.0f, 0.0f);
			_effect.get_parameter("pMaskBase").set_float4(base);
			//Alpha Only Matrix
			matrix4 alphaOnly;
			alphaOnly.t.w = 1; //Only set the alpha channel to be used by the mask.
			_effect.get_parameter("pMaskMatrix").set_matrix(alphaOnly);
			vec4 ones;
			vec4_set(&ones, 1.0f, 1.0f, 1.0f, 1.0f);
			_effect.get_parameter("pMaskMultiplier").set_float4(ones);

			while (gs_effect_loop(_effect.get(), "Mask")) {
				streamfx::gs_draw_fullscreen_tri();
			}

			gs_blend_state_pop();
		} catch (...) {
			obs_source_skip_video_filter(_self);
			return;
		}

		_dirty = false;
	}

	{ // Draw the result for the next filter to use.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_render, "Render"};
#endif

		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), _masked->get_texture()->get_object());
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(nullptr, 0, _out_size.first, _out_size.second);
		}
	}
}

struct switch_provider_data_t {
	background_removal_provider provider;
};

void streamfx::filter::background_removal::background_removal_instance::switch_provider(
	background_removal_provider provider)
{
	std::unique_lock<std::mutex> ul(_provider_lock);

	// Safeguard against calls made from unlocked memory.
	if (provider == _provider) {
		return;
	}

	// This doesn't work correctly.
	// - Need to allow multiple switches at once because OBS is weird.
	// - Doesn't guarantee that the task is properly killed off.

	// Log information.
	D_LOG_INFO("Instance '%s' is switching provider from '%s' to '%s'.", obs_source_get_name(_self), cstring(_provider),
			   cstring(provider));

	// 1.If there is an existing task, attempt to cancel it.
	if (_provider_task) {
		streamfx::threadpool()->pop(_provider_task);
	}

	// 2. Build data to pass into the task.
	auto spd      = std::make_shared<switch_provider_data_t>();
	spd->provider = _provider;
	_provider     = provider;

	// 3. Then spawn a new task to switch provider.
	_provider_task = streamfx::threadpool()->push(
		std::bind(&background_removal_instance::task_switch_provider, this, std::placeholders::_1), spd);
}

void streamfx::filter::background_removal::background_removal_instance::task_switch_provider(
	util::threadpool_data_t data)
{
	std::shared_ptr<switch_provider_data_t> spd = std::static_pointer_cast<switch_provider_data_t>(data);

	// 1. Mark the provider as no longer ready.
	_provider_ready = false;

	// 2. Lock the provider from being used.
	std::unique_lock<std::mutex> ul(_provider_lock);

	try {
		// 3. Unload the previous provider.
		switch (spd->provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		case background_removal_provider::NVIDIA_GREEN_SCREEN:
			nvidia_unload();
			break;
#endif
		default:
			break;
		}

		// 4. Load the new provider.
		switch (_provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		case background_removal_provider::NVIDIA_GREEN_SCREEN:
			nvidia_load();
			{
				auto data = obs_source_get_settings(_self);
				//nvidia_update(data);
				obs_data_release(data);
			}
			break;
#endif
		default:
			break;
		}

		// Log information.
		D_LOG_INFO("Instance '%s' switched provider from '%s' to '%s'.", obs_source_get_name(_self),
				   cstring(spd->provider), cstring(_provider));

		// 5. Set the new provider as valid.
		_provider_ready = true;
	} catch (std::exception const& ex) {
		// Log information.
		D_LOG_ERROR("Instance '%s' failed switching provider with error: %s", obs_source_get_name(_self), ex.what());
	}
}

#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
void streamfx::filter::background_removal::background_removal_instance::nvidia_load()
{
	_nvidia_fx = std::make_shared<::streamfx::nvidia::vfx::background_removal>();
}

void streamfx::filter::background_removal::background_removal_instance::nvidia_unload()
{
	_nvidia_fx.reset();
}

void streamfx::filter::background_removal::background_removal_instance::nvidia_size()
{
	if (!_nvidia_fx) {
		return;
	}

	auto in_size = _in_size;
	_nvidia_fx->size(in_size, _in_size, _out_size);
}

void streamfx::filter::background_removal::background_removal_instance::nvidia_process()
{
	if (!_nvidia_fx) {
		_mask = _input->get_texture();
		return;
	}

	_mask = _nvidia_fx->process(_input->get_texture());
}
/*
void streamfx::filter::background_removal::background_removal_instance::nvidia_properties(
	obs_properties_t* props)
{
	obs_properties_t* grp = obs_properties_create();
	obs_properties_add_group(props, ST_KEY_NVIDIA_SUPERRES, D_TRANSLATE(ST_I18N_NVIDIA_SUPERRES), OBS_GROUP_NORMAL,
							 grp);

	{
		auto p =
			obs_properties_add_list(grp, ST_KEY_NVIDIA_SUPERRES_STRENGTH, D_TRANSLATE(ST_I18N_NVIDIA_SUPERRES_STRENGTH),
									OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_NVIDIA_SUPERRES_STRENGTH_WEAK), 0);
		obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_NVIDIA_SUPERRES_STRENGTH_STRONG), 1);
	}

	{
		auto p = obs_properties_add_float_slider(grp, ST_KEY_NVIDIA_SUPERRES_SCALE,
												 D_TRANSLATE(ST_I18N_NVIDIA_SUPERRES_SCALE), 100.00, 400.00, .01);
		obs_property_float_set_suffix(p, " %");
	}
}

void streamfx::filter::background_removal::background_removal_instance::nvidia_update(obs_data_t* data)
{
	if (!_nvidia_fx)
		return;

	_nvidia_fx->set_strength(
		static_cast<float>(obs_data_get_int(data, ST_KEY_NVIDIA_SUPERRES_STRENGTH) == 0 ? 0. : 1.));
	_nvidia_fx->set_scale(static_cast<float>(obs_data_get_double(data, ST_KEY_NVIDIA_SUPERRES_SCALE) / 100.));
}
*/

#endif

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------
background_removal_factory::~background_removal_factory() {}

background_removal_factory::background_removal_factory()
{
	bool any_available = false;

	// 1. Try and load any configured providers.
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
	try {
		// Load CVImage and Video Effects SDK.
		_nvcuda           = ::streamfx::nvidia::cuda::obs::get();
		_nvcvi            = ::streamfx::nvidia::cv::cv::get();
		_nvvfx            = ::streamfx::nvidia::vfx::vfx::get();
		_nvidia_available = true;
		any_available |= _nvidia_available;
	} catch (const std::exception& ex) {
		_nvidia_available = false;
		_nvvfx.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA Green Screen available due to error: %s", ex.what());
	}
#endif

	// 2. Check if any of them managed to load at all.
	if (!any_available) {
		D_LOG_ERROR("All supported Background Removal providers failed to initialize, disabling effect.", 0);
		return;
	}

	// 3. In any other case, register the filter!
	_info.id           = S_PREFIX "filter-background-removal";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO /*| OBS_SOURCE_SRGB*/;

	set_resolution_enabled(true);
	finish_setup();
}

const char* background_removal_factory::get_name()
{
	return D_TRANSLATE(ST_I18N);
}

void background_removal_factory::get_defaults2(obs_data_t* data)
{
	obs_data_set_default_int(data, ST_KEY_PROVIDER, static_cast<int64_t>(background_removal_provider::AUTOMATIC));

#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
	//obs_data_set_default_double(data, ST_KEY_NVIDIA_SUPERRES_SCALE, 150.);
	//obs_data_set_default_double(data, ST_KEY_NVIDIA_SUPERRES_STRENGTH, 0.);
#endif
}

obs_properties_t* background_removal_factory::get_properties2(background_removal_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

#ifdef ENABLE_FRONTEND
	//{
	//	obs_properties_add_button2(pr, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN),
	//							   background_removal_factory::on_manual_open, nullptr);
	//}
#endif

	if (data) {
		data->properties(pr);
	}

	{ // Advanced Settings
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, S_ADVANCED, D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_KEY_PROVIDER, D_TRANSLATE(ST_I18N_PROVIDER), OBS_COMBO_TYPE_LIST,
											 OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(p, D_TRANSLATE(S_STATE_AUTOMATIC),
									  static_cast<int64_t>(background_removal_provider::AUTOMATIC));
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_PROVIDER_NVIDIA_GREEN_SCREEN),
									  static_cast<int64_t>(background_removal_provider::NVIDIA_GREEN_SCREEN));
		}
	}

	return pr;
}

#ifdef ENABLE_FRONTEND
bool background_removal_factory::on_manual_open(obs_properties_t* props, obs_property_t* property, void* data)
{
	streamfx::open_url(HELP_URL);
	return false;
}
#endif

bool streamfx::filter::background_removal::background_removal_factory::is_provider_available(
	background_removal_provider provider)
{
	switch (provider) {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
	case background_removal_provider::NVIDIA_GREEN_SCREEN:
		return _nvidia_available;
#endif
	default:
		return false;
	}
}

std::shared_ptr<background_removal_factory> _background_removal_factory_instance = nullptr;

void background_removal_factory::initialize()
{
	if (!_background_removal_factory_instance)
		_background_removal_factory_instance = std::make_shared<background_removal_factory>();
}

void background_removal_factory::finalize()
{
	_background_removal_factory_instance.reset();
}

std::shared_ptr<background_removal_factory> background_removal_factory::get()
{
	return _background_removal_factory_instance;
}

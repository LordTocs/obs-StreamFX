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

#include "nvidia-vfx-background-removal.hpp"
#include <utility>
#include "obs/gs/gs-helper.hpp"
#include "util/util-logging.hpp"
#include "util/utility.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<nvidia::vfx::background_removal::background_removal> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

streamfx::nvidia::vfx::background_removal::~background_removal()
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	_fx.reset();

	// Clean up any CUDA resources in use.
	_input.reset();
	_source.reset();
	_destination.reset();
	_output.reset();
	_tmp.reset();

	// Release CUDA, CVImage, and Video Effects SDK.
	_nvvfx.reset();
	_nvcvi.reset();
	_nvcuda.reset();
}

streamfx::nvidia::vfx::background_removal::background_removal()
	: _nvcuda(::streamfx::nvidia::cuda::obs::get()), _nvcvi(::streamfx::nvidia::cv::cv::get()),
	  _nvvfx(::streamfx::nvidia::vfx::vfx::get()), _input(), _source(), _destination(), _output(),
	  _tmp(), _dirty(true)
{
	// Enter Graphics and CUDA context.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	{ // Try & Create the Green Screen effect.
		::streamfx::nvidia::vfx::handle_t handle;
		if (auto res = _nvvfx->NvVFX_CreateEffect(::streamfx::nvidia::vfx::EFFECT_GREEN_SCREEN, &handle);
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to create effect due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("CreateEffect failed.");
		}

		_fx = std::shared_ptr<void>(handle, [](::streamfx::nvidia::vfx::handle_t handle) {
			::streamfx::nvidia::vfx::vfx::get()->NvVFX_DestroyEffect(handle);
		});
	}

	// Assign the appropriate CUDA stream.
	if (auto res = _nvvfx->NvVFX_SetCudaStream(_fx.get(), ::streamfx::nvidia::vfx::PARAMETER_CUDA_STREAM,
											   _nvcuda->get_stream()->get());
		res != ::streamfx::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to set CUDA stream due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("SetCudaStream failed.");
	}

	// Set the proper model directory.
	if (auto res = _nvvfx->NvVFX_SetString(_fx.get(), ::streamfx::nvidia::vfx::PARAMETER_MODEL_DIRECTORY,
										   _nvvfx->model_path().generic_u8string().c_str());
		res != ::streamfx::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to set model directory due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("SetString failed.");
	}

	// Set the strength, scale and buffers.
	resize(160, 90);

	// Load the effect.
	load();
}

void streamfx::nvidia::vfx::background_removal::size(std::pair<uint32_t, uint32_t> const& size,
													 std::pair<uint32_t, uint32_t>&       input_size,
													 std::pair<uint32_t, uint32_t>&       output_size)
{
	output_size.first  = static_cast<uint32_t>(input_size.first);
	output_size.second = static_cast<uint32_t>(input_size.second);
}

std::shared_ptr<::streamfx::obs::gs::texture>
	streamfx::nvidia::vfx::background_removal::process(std::shared_ptr<::streamfx::obs::gs::texture> in)
{
	// Enter Graphics and CUDA context.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = _nvcuda->get_context()->enter();

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_magenta, "NvVFX Super-Resolution"};
#endif

	// Resize if the size or scale was changed.
	resize(in->get_width(), in->get_height());

	// Reload effect if dirty.
	if (_dirty) {
		load();
	}

	{ // Copy parameter to input.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy, "Copy In -> Input"};
#endif
		gs_copy_texture(_input->get_texture()->get_object(), in->get_object());
	}

	{ // Copy input to source.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy, "Copy Input -> Source"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_input->get_image(), _source->get_image(), 1.f,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer input to processing source due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	{ // Process source to destination.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_cache, "Process"};
#endif
		if (auto res = _nvvfx->NvVFX_Run(_fx.get(), 0); res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to process due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Run failed.");
		}
	}

	{ // Copy destination to output.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy,
													"Copy Destination -> Output"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_destination->get_image(), _output->get_image(), 1.,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer processing result to output due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	// Return output.
	return _output->get_texture();
}

void streamfx::nvidia::vfx::background_removal::resize(uint32_t width, uint32_t height)
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	if (!_tmp) {
		_tmp = std::make_shared<::streamfx::nvidia::cv::image>(
			width, height, ::streamfx::nvidia::cv::pixel_format::RGBA, ::streamfx::nvidia::cv::component_type::UINT8,
			::streamfx::nvidia::cv::component_layout::PLANAR, ::streamfx::nvidia::cv::memory_location::GPU, 1);
	}

	// Input Size was changed.
	if (!_input || !_source || (width != _input->get_texture()->get_width())
		|| (height != _input->get_texture()->get_height())) {
		if (_input) {
			_input->resize(width, height);
		} else {
			_input = std::make_shared<::streamfx::nvidia::cv::texture>(width, height, GS_RGBA_UNORM);
		}

		if (_source) {
			_source->resize(width, height);
		} else {
			_source = std::make_shared<::streamfx::nvidia::cv::image>(
				width, height, ::streamfx::nvidia::cv::pixel_format::BGR, ::streamfx::nvidia::cv::component_type::UINT8,
				::streamfx::nvidia::cv::component_layout::CHUNKY, ::streamfx::nvidia::cv::memory_location::GPU, 1);
		}

		if (auto res = _nvvfx->NvVFX_SetImage(_fx.get(), ::streamfx::nvidia::vfx::PARAMETER_INPUT_IMAGE_0,
											  _source->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to set input image due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("SetImage failed.");
		}

		if (auto res = _nvvfx->NvVFX_SetU32(_fx.get(), ::streamfx::nvidia::vfx::PARAMETER_MODE, 0);
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to set mode due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("SetU32 failed.");
		}

		_dirty = true;
	}

	// Input Size or Scale was changed.
	if (!_destination || !_output || (width != _output->get_texture()->get_width())
		|| (height != _output->get_texture()->get_height())) {
		if (_destination) {
			_destination->resize(width, height);
		} else {
			_destination = std::make_shared<::streamfx::nvidia::cv::image>(
				width, height, ::streamfx::nvidia::cv::pixel_format::A, ::streamfx::nvidia::cv::component_type::UINT8,
				::streamfx::nvidia::cv::component_layout::PLANAR, ::streamfx::nvidia::cv::memory_location::GPU, 1);
		}

		if (_output) {
			_output->resize(width, height);
		} else {
			_output = std::make_shared<::streamfx::nvidia::cv::texture>(width, height, GS_A8);
		}

		if (auto res = _nvvfx->NvVFX_SetImage(_fx.get(), ::streamfx::nvidia::vfx::PARAMETER_OUTPUT_IMAGE_0,
											  _destination->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to set output image due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("SetImage failed.");
		}

		_dirty = true;
	}
}

void streamfx::nvidia::vfx::background_removal::load()
{
	auto gctx = ::streamfx::obs::gs::context();
	{
		auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();
		if (auto res = _nvvfx->NvVFX_SetCudaStream(_fx.get(), ::streamfx::nvidia::vfx::PARAMETER_CUDA_STREAM,
												   _nvcuda->get_stream()->get());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to set CUDA stream due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("SetCudaStream failed.");
		}
	}

	{
		auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();
		if (auto res = _nvvfx->NvVFX_Load(_fx.get()); res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to initialize effect due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Load failed.");
		}
	}

	_dirty = false;
}

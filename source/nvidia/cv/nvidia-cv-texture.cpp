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

// NVIDIA CVImage is part of:
// - NVIDIA Video Effects SDK
// - NVIDIA Augmented Reality SDK

#include "nvidia-cv-texture.hpp"
#include "nvidia/cuda/nvidia-cuda-obs.hpp"
#include "obs/gs/gs-helper.hpp"
#include "util/util-logging.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<nvidia::cv::texture> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

using ::nvidia::cv::component_layout;
using ::nvidia::cv::component_type;
using ::nvidia::cv::pixel_format;
using ::nvidia::cv::result;
using ::nvidia::cv::texture;

texture::~texture()
{
	auto gctx  = gs::context();
	auto nvobs = ::nvidia::cuda::obs::get();
	auto cctx  = nvobs->get_context()->enter();

	free();
	_texture.reset();
}

texture::texture(uint32_t width, uint32_t height, gs_color_format pix_fmt)
{
	auto gctx  = gs::context();
	auto nvobs = ::nvidia::cuda::obs::get();
	auto cctx  = nvobs->get_context()->enter();

	// Allocate a new Texture
	_texture = std::make_shared<::gs::texture>(width, height, pix_fmt, 1, nullptr, ::gs::texture::flags::None);
	alloc();
}

void texture::resize(uint32_t width, uint32_t height)
{
	auto gctx  = gs::context();
	auto nvobs = ::nvidia::cuda::obs::get();
	auto cctx  = nvobs->get_context()->enter();

	D_LOG_DEBUG("Resizing object 0x%" PRIxPTR " to %" PRIu32 "x%" PRIu32 "...", this, width, height);

	// Allocate a new Texture
	free();
	_texture = std::make_shared<::gs::texture>(width, height, _texture->get_color_format(), 1, nullptr,
											   ::gs::texture::flags::None);
	alloc();
}

std::shared_ptr<gs::texture> texture::get_texture()
{
	return _texture;
}

void nvidia::cv::texture::alloc()
{
	auto nvobs = ::nvidia::cuda::obs::get();

	// Allocate any relevant CV buffers and Map it.
	if (auto res = _cv->NvCVImage_InitFromD3D11Texture(
			&_image, reinterpret_cast<ID3D11Texture2D*>(gs_texture_get_obj(_texture->get_object())));
		res != result::SUCCESS) {
		D_LOG_ERROR("Object 0x%" PRIxPTR " failed NvCVImage_InitFromD3D11Texture call with error: %s", this,
					_cv->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("NvCVImage_InitFromD3D11Texture");
	}
	if (auto res = _cv->NvCVImage_MapResource(&_image, nvobs->get_stream()->get()); res != result::SUCCESS) {
		D_LOG_ERROR("Object 0x%" PRIxPTR " failed NvCVImage_MapResource call with error: %s", this,
					_cv->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("NvCVImage_MapResource");
	}
}

void nvidia::cv::texture::free()
{
	auto nvobs = ::nvidia::cuda::obs::get();

	// Unmap and deallocate any relevant CV buffers.
	if (auto res = _cv->NvCVImage_UnmapResource(&_image, nvobs->get_stream()->get()); res != result::SUCCESS) {
		D_LOG_ERROR("Object 0x%" PRIxPTR " failed NvCVImage_UnmapResource call with error: %s", this,
					_cv->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("NvCVImage_UnmapResource");
	}
	if (auto res = _cv->NvCVImage_Dealloc(&_image); res != result::SUCCESS) {
		D_LOG_ERROR("Object 0x%" PRIxPTR " failed NvCVImage_Dealloc call with error: %s", this,
					_cv->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("NvCVImage_Dealloc");
	}
}

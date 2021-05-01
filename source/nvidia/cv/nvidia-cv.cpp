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

#include "nvidia-cv.hpp"
#include <filesystem>
#include <mutex>
#include "util/util-logging.hpp"
#include "util/util-platform.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<nvidia::cv::cv> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

#if defined(WIN32)
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Windows.h>

#define LIB_NAME "NVCVImage.dll"
#else
#define LIB_NAME "libNVCVImage.so"
#endif

#define ENV_NVIDIA_AR_SDK_PATH L"NV_AR_SDK_PATH"
#define ENV_NVIDIA_VIDEO_EFFECTS_SDK_PATH L"NV_VIDEO_EFFECTS_PATH"

#define NVCVI_LOAD_SYMBOL(NAME)                                                          \
	{                                                                                    \
		NAME = reinterpret_cast<decltype(NAME)>(_library->load_symbol(#NAME));           \
		if (!NAME)                                                                       \
			throw std::runtime_error("Failed to load '" #NAME "' from '" LIB_NAME "'."); \
	}

nvidia::cv::cv::~cv()
{
	D_LOG_DEBUG("Finalizing... (Addr: 0x%" PRIuPTR ")", this);
}

nvidia::cv::cv::cv()
{
	D_LOG_DEBUG("Initializing... (Addr: 0x%" PRIuPTR ")", this);

	// Try and load the libraries
	try {
		// Try to load it directly first, it may be on the search path already.
		_library = util::library::load(std::string_view(LIB_NAME));
	} catch (...) {
		std::vector<std::filesystem::path> _lib_paths;
		std::filesystem::path              _vfx_sdk_path;
		std::filesystem::path              _ar_sdk_path;

		// 1. Figure out the location of the Video Effects and AR SDK, if they are installed.
#ifdef WIN32
		{
			DWORD                env_size;
			std::vector<wchar_t> buffer;

			env_size = GetEnvironmentVariableW(ENV_NVIDIA_VIDEO_EFFECTS_SDK_PATH, nullptr, 0);
			if (env_size > 0) {
				buffer.resize(static_cast<size_t>(env_size) + 1);
				env_size = GetEnvironmentVariableW(ENV_NVIDIA_VIDEO_EFFECTS_SDK_PATH, buffer.data(), buffer.size());
				_vfx_sdk_path = std::wstring(buffer.data(), buffer.size());
			} else {
				PWSTR   str = nullptr;
				HRESULT res = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &str);
				if (res == S_OK) {
					_vfx_sdk_path = std::wstring(str);
					_vfx_sdk_path /= "NVIDIA Corporation";
					_vfx_sdk_path /= "NVIDIA Video Effects";
					_vfx_sdk_path /= LIB_NAME;
					CoTaskMemFree(str);
				}
			}
			env_size = GetEnvironmentVariableW(ENV_NVIDIA_AR_SDK_PATH, nullptr, 0);
			if (env_size > 0) {
				buffer.resize(static_cast<size_t>(env_size) + 1);
				env_size     = GetEnvironmentVariableW(ENV_NVIDIA_AR_SDK_PATH, buffer.data(), buffer.size());
				_ar_sdk_path = std::wstring(buffer.data(), buffer.size());
			} else {
				PWSTR   str = nullptr;
				HRESULT res = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &str);
				if (res == S_OK) {
					_vfx_sdk_path = std::wstring(str);
					_vfx_sdk_path /= "NVIDIA Corporation";
					_vfx_sdk_path /= "NVIDIA AR SDK";
					_vfx_sdk_path /= LIB_NAME;
					CoTaskMemFree(str);
				}
			}
		}
#else
		throw std::runtime_error("Not yet implemented.");
#endif

		// 2. Check if any of the found paths are valid.
		if (std::filesystem::exists(_vfx_sdk_path)) {
			_lib_paths.push_back(_vfx_sdk_path);
		}
		if (std::filesystem::exists(_ar_sdk_path)) {
			_lib_paths.push_back(_ar_sdk_path);
		}

		// 3. Try and load the library.
		if (_lib_paths.size() > 0) {
			for (auto path : _lib_paths) {
				std::string pathu8;
				try {
					pathu8   = util::platform::native_to_utf8(path.wstring());
					_library = util::library::load(std::string_view(pathu8));
				} catch (...) {
					D_LOG_WARNING("Failed to load '%s' from '%s'.", LIB_NAME, pathu8.c_str());
				}
			}
		} else {
			D_LOG_ERROR("No supported NVIDIA SDK is installed to provide '%s'.", LIB_NAME);
			throw std::runtime_error("Failed to load '" LIB_NAME "'.");
		}

		if (!_library) {
			D_LOG_ERROR("All attempts at loading '%s' have failed.", LIB_NAME);
			throw std::runtime_error("Failed to load '" LIB_NAME "'.");
		}
	}

	{ // Load Symbols
		NVCVI_LOAD_SYMBOL(NvCVImage_Init);
		NVCVI_LOAD_SYMBOL(NvCVImage_InitView);
		NVCVI_LOAD_SYMBOL(NvCVImage_Alloc);
		NVCVI_LOAD_SYMBOL(NvCVImage_Realloc);
		NVCVI_LOAD_SYMBOL(NvCVImage_Dealloc);
		NVCVI_LOAD_SYMBOL(NvCVImage_Create);
		NVCVI_LOAD_SYMBOL(NvCVImage_Destroy);
		NVCVI_LOAD_SYMBOL(NvCVImage_ComponentOffsets);
		NVCVI_LOAD_SYMBOL(NvCVImage_Transfer);
		NVCVI_LOAD_SYMBOL(NvCVImage_TransferRect);
		NVCVI_LOAD_SYMBOL(NvCVImage_TransferFromYUV);
		NVCVI_LOAD_SYMBOL(NvCVImage_TransferToYUV);
		NVCVI_LOAD_SYMBOL(NvCVImage_MapResource);
		NVCVI_LOAD_SYMBOL(NvCVImage_UnmapResource);
		NVCVI_LOAD_SYMBOL(NvCVImage_Composite);
		NVCVI_LOAD_SYMBOL(NvCVImage_CompositeRect);
		NVCVI_LOAD_SYMBOL(NvCVImage_CompositeOverConstant);
		NVCVI_LOAD_SYMBOL(NvCVImage_FlipY);
		NVCVI_LOAD_SYMBOL(NvCVImage_GetYUVPointers);
		NVCVI_LOAD_SYMBOL(NvCV_GetErrorStringFromCode);
#ifdef WIN32
		NVCVI_LOAD_SYMBOL(NvCVImage_InitFromD3D11Texture);
		NVCVI_LOAD_SYMBOL(NvCVImage_ToD3DFormat);
		NVCVI_LOAD_SYMBOL(NvCVImage_FromD3DFormat);
#ifdef __dxgicommon_h__
		NVCVI_LOAD_SYMBOL(NvCVImage_ToD3DColorSpace);
		NVCVI_LOAD_SYMBOL(NvCVImage_FromD3DColorSpace);
#endif
#endif
	}
}

std::shared_ptr<nvidia::cv::cv> nvidia::cv::cv::get()
{
	static std::weak_ptr<nvidia::cv::cv> instance;
	static std::mutex                    lock;

	std::unique_lock<std::mutex> ul(lock);
	if (instance.expired()) {
		auto hard_instance = std::make_shared<nvidia::cv::cv>();
		instance           = hard_instance;
		return hard_instance;
	}
	return instance.lock();
}

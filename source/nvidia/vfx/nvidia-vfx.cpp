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

#include "nvidia-vfx.hpp"
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
#define ST_PREFIX "<nvidia::vfx::vfx> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

#if defined(WIN32)
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Windows.h>

#define LIB_NAME "NVVideoEffects.dll"
#else
#define LIB_NAME "libNVVideoEffects.so"
#endif

#define ENV_NVIDIA_VIDEO_EFFECTS_SDK_PATH L"NV_VIDEO_EFFECTS_PATH"

#define NVVFX_LOAD_SYMBOL(NAME)                                                          \
	{                                                                                    \
		NAME = reinterpret_cast<decltype(NAME)>(_library->load_symbol(#NAME));           \
		if (!NAME)                                                                       \
			throw std::runtime_error("Failed to load '" #NAME "' from '" LIB_NAME "'."); \
	}

nvidia::vfx::vfx::~vfx()
{
	D_LOG_DEBUG("Finalizing... (Addr: 0x%" PRIuPTR ")", this);
}

nvidia::vfx::vfx::vfx()
{
	D_LOG_DEBUG("Initializing... (Addr: 0x%" PRIuPTR ")", this);

	// Try and load the libraries
	try {
		// Try to load it directly first, it may be on the search path already.
		_library = util::library::load(std::string_view(LIB_NAME));
	} catch (...) {
		std::filesystem::path _vfx_sdk_path;

		// 1. Figure out the location of the Video Effects SDK, if they are installed.
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
		}
#else
		throw std::runtime_error("Not yet implemented.");
#endif

		// 2. Check if any of the found paths are valid.
		if (!std::filesystem::exists(_vfx_sdk_path)) {
			D_LOG_ERROR("No supported NVIDIA SDK is installed to provide '%s'.", LIB_NAME);
			throw std::runtime_error("Failed to load '" LIB_NAME "'.");
		}

		// 3. Try and load the library.
		std::string pathu8;
		try {
			pathu8   = util::platform::native_to_utf8(_vfx_sdk_path.wstring());
			_library = util::library::load(std::string_view(pathu8));
		} catch (...) {
			D_LOG_ERROR("Failed to load '%s' from '%s'.", LIB_NAME, pathu8.c_str());
			throw std::runtime_error("Failed to load '" LIB_NAME "'.");
		}
	}

	{ // Load Symbols
		NVVFX_LOAD_SYMBOL(NvVFX_GetVersion);
		NVVFX_LOAD_SYMBOL(NvVFX_CreateEffect);
		NVVFX_LOAD_SYMBOL(NvVFX_DestroyEffect);
		NVVFX_LOAD_SYMBOL(NvVFX_SetU32);
		NVVFX_LOAD_SYMBOL(NvVFX_SetS32);
		NVVFX_LOAD_SYMBOL(NvVFX_SetF32);
		NVVFX_LOAD_SYMBOL(NvVFX_SetF64);
		NVVFX_LOAD_SYMBOL(NvVFX_SetU64);
		NVVFX_LOAD_SYMBOL(NvVFX_SetImage);
		NVVFX_LOAD_SYMBOL(NvVFX_SetObject);
		NVVFX_LOAD_SYMBOL(NvVFX_SetString);
		NVVFX_LOAD_SYMBOL(NvVFX_SetCudaStream);
		NVVFX_LOAD_SYMBOL(NvVFX_GetU32);
		NVVFX_LOAD_SYMBOL(NvVFX_GetS32);
		NVVFX_LOAD_SYMBOL(NvVFX_GetF32);
		NVVFX_LOAD_SYMBOL(NvVFX_GetF64);
		NVVFX_LOAD_SYMBOL(NvVFX_GetU64);
		NVVFX_LOAD_SYMBOL(NvVFX_GetImage);
		NVVFX_LOAD_SYMBOL(NvVFX_GetObject);
		NVVFX_LOAD_SYMBOL(NvVFX_GetString);
		NVVFX_LOAD_SYMBOL(NvVFX_GetCudaStream);
		NVVFX_LOAD_SYMBOL(NvVFX_Run);
		NVVFX_LOAD_SYMBOL(NvVFX_Load);
	}
}

std::shared_ptr<::nvidia::vfx::vfx> nvidia::vfx::vfx::get()
{
	static std::weak_ptr<nvidia::vfx::vfx> instance;
	static std::mutex                      lock;

	std::unique_lock<std::mutex> ul(lock);
	if (instance.expired()) {
		auto hard_instance = std::make_shared<nvidia::vfx::vfx>();
		instance           = hard_instance;
		return hard_instance;
	}
	return instance.lock();
}

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

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "obs/gs/gs-rendertarget.hpp"
#include "obs/gs/gs-texture.hpp"
#include "obs/obs-source-factory.hpp"
#include "plugin.hpp"
#include "util/util-threadpool.hpp"

#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
#include "nvidia/cuda/nvidia-cuda-obs.hpp"
#include "nvidia/cv/nvidia-cv.hpp"
#include "nvidia/vfx/nvidia-vfx.hpp"
#endif

namespace streamfx::filter::video_denoising {
	enum denoise_provider {
		AUTOMATIC                  = 0,
		NVIDIA_VIDEO_NOISE_REMOVAL = 1,
	};

	const char* denoise_provider_to_string(denoise_provider provider);

	class video_denoising_instance : public obs::source_instance {
		std::pair<uint32_t, uint32_t> _size;

		std::atomic<bool>                       _provider_ready;
		denoise_provider                        _provider;
		std::mutex                              _provider_lock;
		std::shared_ptr<util::threadpool::task> _provider_task;

		std::shared_ptr<gs::rendertarget> _input;
		std::shared_ptr<gs::texture>      _output;

#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
		std::shared_ptr<::nvidia::cuda::obs> _nvcuda = nullptr;
		std::shared_ptr<::nvidia::cv::cv>    _nvcvi  = nullptr;
		std::shared_ptr<::nvidia::vfx::vfx>  _nvvfx  = nullptr;

		std::shared_ptr<gs::texture> _nvidia_input  = nullptr;
		std::shared_ptr<gs::texture> _nvidia_output = nullptr;

		::nvidia::cv::image_t _nvidia_cvi_input  = {};
		::nvidia::cv::image_t _nvidia_cvi_output = {};
#endif

		public:
		video_denoising_instance(obs_data_t* data, obs_source_t* self);
		~video_denoising_instance() override;

		void load(obs_data_t* data) override;
		void migrate(obs_data_t* data, uint64_t version) override;
		void update(obs_data_t* data) override;

		uint32_t get_width() override;
		uint32_t get_height() override;

		void video_tick(float_t time) override;
		void video_render(gs_effect_t* effect) override;

		private:
		void switch_provider(denoise_provider provider);
		void task_switch_provider(util::threadpool_data_t data);

#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
		void                          load_nvidia_noise_removal();
		void                          unload_nvidia_noise_removal();
		std::pair<uint32_t, uint32_t> resize_nvidia_noise_removal(uint32_t width, uint32_t height);
		std::shared_ptr<gs::texture>  process_nvidia_noise_removal();
#endif
	};

	class video_denoising_factory
		: public obs::source_factory<::streamfx::filter::video_denoising::video_denoising_factory,
									 ::streamfx::filter::video_denoising::video_denoising_instance> {
#ifdef ENABLE_FILTER_VIDEO_DENOISING_NVIDIA
		bool                                 _nvidia_available;
		std::shared_ptr<::nvidia::cuda::obs> _nvcuda;
		std::shared_ptr<::nvidia::cv::cv>    _nvcvi;
		std::shared_ptr<::nvidia::vfx::vfx>  _nvvfx;
#endif

		public:
		virtual ~video_denoising_factory();
		video_denoising_factory();

		virtual const char* get_name() override;

		virtual void              get_defaults2(obs_data_t* data) override;
		virtual obs_properties_t* get_properties2(video_denoising_instance* data) override;

#ifdef ENABLE_FRONTEND
		static bool on_manual_open(obs_properties_t* props, obs_property_t* property, void* data);
#endif

		bool is_provider_available(denoise_provider);

		public: // Singleton
		static void                                                                          initialize();
		static void                                                                          finalize();
		static std::shared_ptr<::streamfx::filter::video_denoising::video_denoising_factory> get();
	};

} // namespace streamfx::filter::video_denoising

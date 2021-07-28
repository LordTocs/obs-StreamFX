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

#pragma once
#include "common.hpp"
#include <atomic>
#include <vector>
#include "obs/gs/gs-effect.hpp"
#include "obs/gs/gs-rendertarget.hpp"
#include "obs/gs/gs-vertexbuffer.hpp"
#include "obs/obs-source-factory.hpp"

// Nvidia
#include "nvidia/ar/nvidia-ar.hpp"
#include "nvidia/cuda/nvidia-cuda-context.hpp"
#include "nvidia/cuda/nvidia-cuda-gs-texture.hpp"
#include "nvidia/cuda/nvidia-cuda-memory.hpp"
#include "nvidia/cuda/nvidia-cuda-obs.hpp"
#include "nvidia/cuda/nvidia-cuda-stream.hpp"
#include "nvidia/cuda/nvidia-cuda.hpp"
#include "nvidia/cv/nvidia-cv.hpp"
#include "nvidia/vfx/nvidia-vfx.hpp"

namespace streamfx::filter::nvidia {
	class background_removal_instance : public obs::source_instance {
		// Filter Cache
		std::pair<uint32_t, uint32_t>     _size;
		bool                              _rt_is_fresh;
		std::shared_ptr<gs::rendertarget> _rt;

		std::mutex _delete_protection;

		// Settings
		//double_t                      _cfg_zoom;
		//std::pair<double_t, double_t> _cfg_offset;
		//double_t                      _cfg_stability;

		// Operational Data
		//TODO: Use smart pointer dumb dumb.
		handle_t                          _nv_effect;
		std::shared_ptr<gs::rendertarget> _input; //RT for capturing the input.

		// Nvidia CUDA interop
		std::shared_ptr<::nvidia::cuda::obs>    _nvcuda = nullptr;
		std::shared_ptr<::nvidia::cv::cv>       _nvcvi  = nullptr;
		std::shared_ptr<::nvidia::vfx::vfx>     _nvvfx  = nullptr;
		std::shared_ptr<::nvidia::cuda::stream> _cuda_stream;

		std::shared_ptr<gs::texture> _nvidia_input  = nullptr;
		std::shared_ptr<gs::texture> _nvidia_output = nullptr;

		::nvidia::cv::image_t _nvidia_cvi_input  = {};
		::nvidia::cv::image_t _nvidia_cvi_output = {};

		::nvidia::cv::image_t _nvidia_cvi_working_input  = {};
		::nvidia::cv::image_t _nvidia_cvi_working_output  = {};

#ifdef ENABLE_PROFILING
		// Profiling
		std::shared_ptr<util::profiler> _profile_capture;
		std::shared_ptr<util::profiler> _profile_capture_realloc;
		std::shared_ptr<util::profiler> _profile_capture_copy;
		std::shared_ptr<util::profiler> _profile_ar_realloc;
		std::shared_ptr<util::profiler> _profile_ar_copy;
		std::shared_ptr<util::profiler> _profile_ar_transfer;
		std::shared_ptr<util::profiler> _profile_ar_run;
		std::shared_ptr<util::profiler> _profile_ar_calc;
#endif

		public:
		background_removal_instance(obs_data_t*, obs_source_t*);
		virtual ~background_removal_instance() override;

		uint32_t get_width() override;
		uint32_t get_height() override;

		virtual void video_tick(float_t seconds) override;
		virtual void video_render(gs_effect_t* effect) override;

#ifdef ENABLE_PROFILING
		bool button_profile(obs_properties_t* props, obs_property_t* property);
#endif
		private:
		std::pair<uint32_t, uint32_t> _enforce_size_nvidia_background_removal(uint32_t x, uint32_t y);
		void ensure_nvidia_rt(std::shared_ptr<gs::texture>& texture, ::nvidia::cv::image_t& nvimg, ::nvidia::cv::image_t& nvimg_working);
		std::shared_ptr<gs::texture> process_nvidia_background_removal();
	};

	class background_removal_factory : public obs::source_factory<filter::nvidia::background_removal_factory,
																  filter::nvidia::background_removal_instance> {
		bool                                 _nvidia_available;
		std::shared_ptr<::nvidia::cuda::obs> _nvcuda;
		std::shared_ptr<::nvidia::cv::cv>    _nvcvi;
		std::shared_ptr<::nvidia::vfx::vfx>  _nvvfx;

		public:
		background_removal_factory();
		virtual ~background_removal_factory() override;

		virtual const char* get_name() override;

		virtual void get_defaults2(obs_data_t* data) override;

		virtual obs_properties_t* get_properties2(background_removal_instance* data) override;

		std::shared_ptr<::nvidia::vfx::vfx> get_vfx();

		public: // Singleton
		static void initialize();

		static void finalize();

		static std::shared_ptr<background_removal_factory> get();
	};
} // namespace streamfx::filter::nvidia

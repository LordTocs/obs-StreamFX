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
#include "obs/gs/gs-effect.hpp"
#include "obs/gs/gs-rendertarget.hpp"
#include "obs/gs/gs-texture.hpp"
#include "obs/obs-source-factory.hpp"
#include "plugin.hpp"
#include "util/util-threadpool.hpp"

#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
#include "nvidia/vfx/nvidia-vfx-background-removal.hpp"
#endif

namespace streamfx::filter::background_removal {
	enum class background_removal_provider {
		INVALID             = -1,
		AUTOMATIC           = 0,
		NVIDIA_GREEN_SCREEN = 1,
	};

	const char* cstring(background_removal_provider provider);

	std::string string(background_removal_provider provider);

	class background_removal_instance : public ::streamfx::obs::source_instance {
		std::pair<uint32_t, uint32_t> _in_size;
		std::pair<uint32_t, uint32_t> _out_size;

		streamfx::obs::gs::effect _effect;

		std::atomic<bool>                        _provider_ready;
		std::atomic<background_removal_provider> _provider;
		std::mutex                               _provider_lock;
		std::shared_ptr<util::threadpool::task>  _provider_task;

		std::shared_ptr<::streamfx::obs::gs::rendertarget> _input;
		std::shared_ptr<::streamfx::obs::gs::texture>      _mask;
		std::shared_ptr<::streamfx::obs::gs::rendertarget> _masked;
		bool                                               _dirty;

#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		std::shared_ptr<::streamfx::nvidia::vfx::background_removal> _nvidia_fx;
#endif

		public:
		background_removal_instance(obs_data_t* data, obs_source_t* self);
		~background_removal_instance() override;

		void load(obs_data_t* data) override;
		void migrate(obs_data_t* data, uint64_t version) override;
		void update(obs_data_t* data) override;
		void properties(obs_properties_t* properties);

		uint32_t get_width() override;
		uint32_t get_height() override;

		void video_tick(float_t time) override;
		void video_render(gs_effect_t* effect) override;

		private:
		void switch_provider(background_removal_provider provider);
		void task_switch_provider(util::threadpool_data_t data);

#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		void nvidia_load();
		void nvidia_unload();
		void nvidia_size();
		void nvidia_process();
		/*void nvidia_properties(obs_properties_t* props);
		void nvidia_update(obs_data_t* data);*/
#endif
	};

	class background_removal_factory
		: public ::streamfx::obs::source_factory<::streamfx::filter::background_removal::background_removal_factory,
												 ::streamfx::filter::background_removal::background_removal_instance> {
#ifdef ENABLE_FILTER_BACKGROUND_REMOVAL_NVIDIA
		bool                                           _nvidia_available;
		std::shared_ptr<::streamfx::nvidia::cuda::obs> _nvcuda;
		std::shared_ptr<::streamfx::nvidia::cv::cv>    _nvcvi;
		std::shared_ptr<::streamfx::nvidia::vfx::vfx>  _nvvfx;
#endif

		public:
		virtual ~background_removal_factory();
		background_removal_factory();

		virtual const char* get_name() override;

		virtual void              get_defaults2(obs_data_t* data) override;
		virtual obs_properties_t* get_properties2(background_removal_instance* data) override;

#ifdef ENABLE_FRONTEND
		static bool on_manual_open(obs_properties_t* props, obs_property_t* property, void* data);
#endif

		bool is_provider_available(background_removal_provider);

		public: // Singleton
		static void                                                                                initialize();
		static void                                                                                finalize();
		static std::shared_ptr<::streamfx::filter::background_removal::background_removal_factory> get();
	};

} // namespace streamfx::filter::background_removal

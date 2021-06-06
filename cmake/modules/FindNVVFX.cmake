# Nvidia AR SDK
# 
# Sets
# - NVVFX_FOUND
# - NVVFX_INCLUDE_DIRS
# - NVVFX_SOURCE_DIRS
#
#

include(FindPackageHandleStandardArgs)
find_package(PkgConfig QUIET)

# Variables
set(NVVFX_ROOT "" CACHE PATH "Path to NVidia VFX SDK")

find_path(NVVFX_INCLUDE_DIRS
	NAMES
		"nvVideoEffects.h" 
	HINTS
		ENV NVVFX_ROOT
		${NVVFX_ROOT}
	PATHS
		/usr/include
		/usr/local/include
		/opt/local/include
	PATH_SUFFIXES
		include
		nvvfx/include
)
find_path(NVVFX_SOURCE_DIRS
	NAMES
		"NVVideoEffectsProxy.cpp"
	HINTS
		ENV NVVFX_ROOT
		${NVVFX_ROOT}
	PATHS
		/usr/include
		/usr/local/include
		/opt/local/include
	PATH_SUFFIXES
		src
		nvvfx/src
)

find_package_handle_standard_args(NVVFX
	FOUND_VAR NVVFX_FOUND
	REQUIRED_VARS NVVFX_INCLUDE_DIRS NVVFX_SOURCE_DIRS
	VERSION_VAR NVVFX_VERSION
	HANDLE_COMPONENTS
)

if(NVVFX_FOUND AND NOT TARGET nvVFXProxy)
	add_library(nvVFXProxy INTERFACE)
	target_include_directories(nvVFXProxy
		INTERFACE
			${NVVFX_SOURCE_DIRS}
			${NVVFX_INCLUDE_DIRS}
	)
endif()

cmake_minimum_required(VERSION 3.21)

# Must be set before project(), which is when the vcpkg toolchain runs.
# The default x64-windows triplet builds spdlog and fmt as DLLs, which an SKSE plugin
# would then have to ship next to itself. static-md links them in while still using the
# dynamic CRT the game uses.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
	set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

project(
	CursorUnbound
	VERSION 1.0.2
	DESCRIPTION "Decouples the Skyrim menu cursor from the game frame rate."
	LANGUAGES CXX RC
)

set(PROJECT_AUTHOR "kevin")
set(PROJECT_FRIENDLY_NAME "Cursor Unbound")

# Stamps the DLL so a build can be identified from a crash log.
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
	"${CMAKE_CURRENT_BINARY_DIR}/version.rc"
	@ONLY
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CommonLibSSE-NG is built for SE + AE only. Enabling VR turns on SKYRIM_CROSS_VR, which
# strips MenuEventHandler off CursorMenu (see RE/C/CursorMenu.h) - and that virtual is the
# whole hook point for this plugin. VR would need a separate strategy, so it stays off.
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG" CommonLibSSE-NG)

set(SOURCES
	src/PCH.h
	src/Config.h
	src/Config.cpp
	src/CursorImage.h
	src/CursorImage.cpp
	src/CursorUnbound.h
	src/CursorUnbound.cpp
	src/main.cpp
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCES})

add_library("${PROJECT_NAME}" SHARED ${SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)

# The version lives in project() above and nowhere else. main.cpp reads it from here for
# both the SKSE plugin record and the log banner, and package.ps1 parses it out of this
# file, so a release archive can never end up labelled differently from the DLL inside it.
target_compile_definitions(
	"${PROJECT_NAME}"
	PRIVATE
		UNICODE
		_UNICODE
		NOMINMAX
		WIN32_LEAN_AND_MEAN
		CURSOR_UNBOUND_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
		CURSOR_UNBOUND_VERSION_MINOR=${PROJECT_VERSION_MINOR}
		CURSOR_UNBOUND_VERSION_PATCH=${PROJECT_VERSION_PATCH}
		CURSOR_UNBOUND_VERSION_STRING="${PROJECT_VERSION}"
)

target_include_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_precompile_headers("${PROJECT_NAME}" PRIVATE src/PCH.h)

target_link_libraries(
	"${PROJECT_NAME}"
	PRIVATE
		CommonLibSSE::CommonLibSSE
		windowscodecs.lib
		Shlwapi.lib
)

if(MSVC)
	target_compile_options(
		"${PROJECT_NAME}"
		PRIVATE
			/EHsc
			/MP
			/W4
			/permissive-
			/Zc:preprocessor
			/utf-8
	)
endif()

set_target_properties(
	"${PROJECT_NAME}"
	PROPERTIES
		OUTPUT_NAME "CursorUnbound"
		SUFFIX ".dll"
)

# Stage into dist/ so the tree mirrors the MO2 mod layout exactly.
add_custom_command(
	TARGET "${PROJECT_NAME}"
	POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"$<TARGET_FILE:${PROJECT_NAME}>"
		"${CMAKE_SOURCE_DIR}/dist/SKSE/Plugins/CursorUnbound.dll"
	COMMENT "Staging CursorUnbound.dll into dist/SKSE/Plugins"
)


cmake_minimum_required(VERSION 3.25)
project(dantes_inferno LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-mssse3 -msse4.1)
endif()

option(DANTESINFERNO_NATIVE_RENDERER "Build native DiligentCore/Vulkan renderer" ON)
option(DANTESINFERNO_FIDELITYFX "Enable AMD FidelityFX present effects (CAS/FSR)" ON)

if(DANTESINFERNO_FIDELITYFX)
    set(REXGLUE_ENABLE_FIDELITYFX ON CACHE BOOL "Enable AMD FidelityFX integration" FORCE)
endif()

if(DANTESINFERNO_NATIVE_RENDERER)
    # dante_gpu compiles the SDK's Vulkan backend in-process; the Vulkan UI
    # objects (provider/presenter/device) must exist inside rexruntime.
    set(REXGLUE_USE_VULKAN ON CACHE BOOL "Enable Vulkan graphics backend" FORCE)
endif()

include(generated/rexglue.cmake)

if(REXSDK_DIR)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp"
        APPEND
        DEPENDS
            "${REXSDK_DIR}/src/codegen/builders/vector.cpp"
            "${REXSDK_DIR}/include/rex/ppc/intrinsics.h"
    )
endif()

set(DANTESINFERNO_SOURCES
    src/main.cpp
    src/frame_rate_hooks.cpp
    # D3D hooks: strong overrides of the generated weak aliases, so in the exe.
    src/renderer/d3d_hooks.cpp
    src/renderer/renderdoc_capture.cpp
    src/renderer/state_verifier.cpp
    src/renderer/guest_gpu_state.cpp
)

if(WIN32)
    list(APPEND DANTESINFERNO_SOURCES src/dantes_inferno.rc)
    add_executable(dantes_inferno WIN32 ${DANTESINFERNO_SOURCES})
    # A PDB in every build, so crash reports' addresses map to function names
    # (archived per release, not shipped).
    target_link_options(dantes_inferno PRIVATE "LINKER:/DEBUG")
else()
    add_executable(dantes_inferno ${DANTESINFERNO_SOURCES})
endif()

rexglue_setup_target(dantes_inferno)

target_include_directories(dantes_inferno PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/thirdparty/imgui"
)

if(TARGET imgui::imgui)
    target_link_libraries(dantes_inferno PRIVATE imgui::imgui)
elseif(TARGET imgui)
    target_link_libraries(dantes_inferno PRIVATE imgui)
endif()

if(DANTESINFERNO_NATIVE_RENDERER)
    # Resolve relative/stale cache values against the source tree so a bad
    # cached path can never break configure.
    if(NOT DILIGENT_CORE_DIR OR NOT IS_ABSOLUTE "${DILIGENT_CORE_DIR}")
        set(DILIGENT_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/diligent-core"
            CACHE PATH "Path to DiligentCore source tree" FORCE)
    endif()

    if(NOT EXISTS "${DILIGENT_CORE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "DANTESINFERNO_NATIVE_RENDERER=ON but DiligentCore not found at:\n"
            "  ${DILIGENT_CORE_DIR}\n"
            "Clone it with:\n"
            "  git clone --depth 1 --recurse-submodules "
            "https://github.com/DiligentGraphics/DiligentCore.git "
            "thirdparty/diligent-core")
    endif()

    set(DILIGENT_NO_HLSL ON CACHE BOOL "Disable HLSL support in DiligentCore" FORCE)
    set(DILIGENT_INSTALL_CORE OFF CACHE BOOL "Don't install DiligentCore" FORCE)
    set(DILIGENT_BUILD_TESTS OFF CACHE BOOL "Don't build DiligentCore tests" FORCE)

    # The SDK's Vulkan stack (REXGLUE_USE_VULKAN) brings its own glslang /
    # SPIRV-Headers targets whose names collide with DiligentCore's vendored
    # copies (target names are global). DiligentCore is therefore built without
    # glslang/SPIRV-Tools; GLSL is compiled to SPIR-V at runtime with the SDK's
    # glslang in vulkan_device.cpp instead.
    set(DILIGENT_NO_GLSLANG ON CACHE BOOL "Disable glslang in DiligentCore" FORCE)

    add_subdirectory("${DILIGENT_CORE_DIR}" "${CMAKE_BINARY_DIR}/diligent-core" EXCLUDE_FROM_ALL)

    add_library(dante_renderer STATIC
        src/renderer/renderer.cpp
        src/renderer/draw_renderer.cpp
        src/renderer/png_dump.cpp
        src/renderer/button_glyphs.cpp
        src/renderer/shader_cache.cpp
        src/renderer/smaa_pass.cpp
        src/renderer/texture_cache.cpp
        src/renderer/renderer_config.cpp
        src/renderer/vulkan_device.cpp
    )
    target_include_directories(dante_renderer PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_include_directories(dante_renderer PRIVATE
        "${DILIGENT_CORE_DIR}"
        "${DILIGENT_CORE_DIR}/Graphics"
        "${DILIGENT_CORE_DIR}/Graphics/GraphicsEngine/interface"
        "${DILIGENT_CORE_DIR}/Graphics/GraphicsEngineVulkan/interface"
        "${DILIGENT_CORE_DIR}/Platforms/interface"
        "${DILIGENT_CORE_DIR}/Platforms/Win32/interface"
        "${DILIGENT_CORE_DIR}/Primitives/interface"
        "${DILIGENT_CORE_DIR}/Common/interface"
    )
    target_link_libraries(dante_renderer PRIVATE
        Diligent-GraphicsEngineVk-static
        Diligent-GraphicsTools
        Diligent-Common
        rex::runtime
        volk::volk
        Vulkan::Headers
    )

    # dante_gpu: the SDK's Xenos graphics backend compiled in-process
    # (core + SPIR-V + Vulkan), replacing the runtime-loaded rexgpu-xenos.dll
    # when renderer=native. Source list mirrors src/graphics/CMakeLists.txt
    # minus plugin_main.cpp and the D3D12/DXBC backend.
    if(REXSDK_DIR)
        set(DANTE_GPU_SOURCES
            # Core GPU types
            ${REXSDK_DIR}/src/graphics/flags.cpp
            ${REXSDK_DIR}/src/graphics/register_file.cpp
            ${REXSDK_DIR}/src/graphics/registers.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/shader.cpp
            ${REXSDK_DIR}/src/graphics/format/ucode.cpp
            ${REXSDK_DIR}/src/graphics/sampler_info.cpp
            # GPU system
            ${REXSDK_DIR}/src/graphics/graphics_system.cpp
            ${REXSDK_DIR}/src/graphics/command_processor.cpp
            ${REXSDK_DIR}/src/graphics/util/draw_extent_estimator.cpp
            ${REXSDK_DIR}/src/graphics/util/draw.cpp
            ${REXSDK_DIR}/src/graphics/packet_disassembler.cpp
            ${REXSDK_DIR}/src/graphics/primitive_processor.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/render_target/cache.cpp
            ${REXSDK_DIR}/src/graphics/shared_memory.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/texture/cache.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/translator.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/translator_disasm.cpp
            # SPIR-V shader translation
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv_builder.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv_translator.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv_translator_alu.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp
            ${REXSDK_DIR}/src/graphics/pipeline/shader/spirv_translator_rb.cpp
            # Vulkan backend
            ${REXSDK_DIR}/src/graphics/vulkan/graphics_system.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/deferred_command_buffer.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/command_processor.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/pipeline_cache.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/primitive_processor.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/render_target_cache.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/shader.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/shared_memory.cpp
            ${REXSDK_DIR}/src/graphics/vulkan/texture_cache.cpp
        )
        add_library(dante_gpu STATIC ${DANTE_GPU_SOURCES})
        target_include_directories(dante_gpu PRIVATE
            ${REXSDK_DIR}
            ${REXSDK_DIR}/thirdparty/renderdoc
        )
        target_compile_options(dante_gpu PRIVATE -Wno-unused-variable)
        target_link_libraries(dante_gpu PRIVATE
            rex::runtime
            xxHash::xxhash
            Vulkan::Headers
            glslang::SPIRV
            GPUOpen::VulkanMemoryAllocator
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
        )
        target_link_libraries(dante_renderer PRIVATE dante_gpu)
        # GDI renders the button glyph labels (button_glyphs.cpp).
        target_link_libraries(dante_renderer PRIVATE gdi32)

        # glslang for runtime GLSL->SPIR-V compilation in vulkan_device.cpp
        # (DiligentCore is built with DILIGENT_NO_GLSLANG). ResourceLimits.cpp
        # provides glslang::DefaultTBuiltInResource, normally built by glslang's
        # StandAlone dir which ENABLE_GLSLANG_BINARIES=OFF skips.
        target_sources(dante_renderer PRIVATE
            ${REXSDK_DIR}/thirdparty/glslang/StandAlone/ResourceLimits.cpp
        )
        target_include_directories(dante_renderer PRIVATE
            ${REXSDK_DIR}/thirdparty/glslang
            ${REXSDK_DIR}/thirdparty/glslang/StandAlone
            ${REXSDK_DIR}/thirdparty/renderdoc
        )
        target_link_libraries(dante_renderer PRIVATE
            xxHash::xxhash
            glslang::SPIRV
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
        )
    else()
        message(WARNING "dante_gpu requires REXSDK_DIR (SDK source tree); "
                        "renderer=native will be unavailable")
    endif()

    target_link_libraries(dantes_inferno PRIVATE dante_renderer)
    target_compile_definitions(dantes_inferno PRIVATE DANTESINFERNO_NATIVE_RENDERER)

    set(DANTESINFERNO_NATIVE_SOURCES
        src/renderer/dantes_inferno_native_main.cpp
        src/renderer/d3d_hooks.cpp
        src/renderer/renderdoc_capture.cpp
        src/renderer/state_verifier.cpp
        src/renderer/guest_gpu_state.cpp
    )
    if(WIN32)
        list(APPEND DANTESINFERNO_NATIVE_SOURCES src/dantes_inferno.rc)
        add_executable(dantes_inferno_native WIN32 ${DANTESINFERNO_NATIVE_SOURCES})
    else()
        add_executable(dantes_inferno_native ${DANTESINFERNO_NATIVE_SOURCES})
    endif()
    rexglue_setup_target(dantes_inferno_native)
    target_include_directories(dantes_inferno_native PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/thirdparty/imgui"
    )
    target_link_libraries(dantes_inferno_native PRIVATE dante_renderer)
    target_compile_definitions(dantes_inferno_native PRIVATE DANTESINFERNO_NATIVE_RENDERER)

endif()

# Qt launcher for the native Linux build (packaging/appimage).
option(DANTESINFERNO_BUILD_LAUNCHER "Build the Qt launcher (Linux)" ON)
option(DANTESINFERNO_LAUNCHER_BUILD_TESTS "Build the Qt launcher tests" ON)
if(DANTESINFERNO_BUILD_LAUNCHER AND UNIX AND NOT APPLE)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets QUIET)
    if(Qt6_FOUND)
        if(DANTESINFERNO_LAUNCHER_BUILD_TESTS)
            enable_testing()
        endif()
        add_subdirectory(launcher-linux)
    else()
        message(WARNING "Qt 6.4+ not found; dantes_inferno_launcher will not be built")
    endif()
endif()

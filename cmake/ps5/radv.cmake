# RADV for the PS5 (mihawk-99/PS5_Mesa, -Dradv-winsys=ps5) as WoWPS's Vulkan
# implementation. The renderer compiles against the Khronos headers RADV was
# built with; the archive itself is linked whole by tools/ps5/link.sh, because
# Mesa's dispatch tables reach their entry points through weak references.
#
# RADV exports only vk_icdGetInstanceProcAddr, so the vk* prototypes the
# renderer, VMA and imgui call are defined by a generated static loader.

if(NOT DEFINED PS5_MESA)
    get_filename_component(PS5_MESA "${PS5_VULKAN}/../PS5_Mesa" ABSOLUTE)
endif()
set(PS5_MESA "${PS5_MESA}" CACHE PATH "PS5_Mesa checkout (for the Vulkan registry, vk.xml)")
set(WOWEE_PS5_VK_XML "${PS5_MESA}/src/vulkan/registry/vk.xml")
if(NOT EXISTS "${WOWEE_PS5_VK_XML}")
    message(FATAL_ERROR "Vulkan registry not found: ${WOWEE_PS5_VK_XML}")
endif()

set(WOWEE_PS5_VK_LOADER "${CMAKE_BINARY_DIR}/generated/vk_ps5_loader.c")
add_custom_command(
    OUTPUT "${WOWEE_PS5_VK_LOADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/ps5/gen_vk_loader.py"
            "${WOWEE_PS5_VK_XML}" "${WOWEE_PS5_VK_LOADER}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/ps5/gen_vk_loader.py" "${WOWEE_PS5_VK_XML}"
    COMMENT "Generating the static Vulkan loader for RADV"
    VERBATIM)

add_library(wowee_ps5_vulkan STATIC "${WOWEE_PS5_VK_LOADER}")
target_include_directories(wowee_ps5_vulkan SYSTEM PUBLIC "${PS5_RADV}/include")
target_compile_definitions(wowee_ps5_vulkan PUBLIC VK_USE_PLATFORM_DISPLAY_KHR=1)
set_target_properties(wowee_ps5_vulkan PROPERTIES C_STANDARD 11 LINKER_LANGUAGE C)
add_library(Vulkan::Vulkan ALIAS wowee_ps5_vulkan)

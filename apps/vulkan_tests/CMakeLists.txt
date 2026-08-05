add_executable(openmw-vulkan-smoke renderer_smoke.cpp)

target_link_libraries(openmw-vulkan-smoke
    PRIVATE
    components
)

target_compile_definitions(openmw-vulkan-smoke
    PRIVATE
    OPENMW_VULKAN_SHADER_DIR="${VK_SHADER_OUTPUT_DIR}"
)

add_dependencies(openmw-vulkan-smoke VulkanShaders)

if (BUILD_TESTING)
    add_test(
        NAME openmw-vulkan-smoke
        COMMAND openmw-vulkan-smoke "${VK_SHADER_OUTPUT_DIR}" 3
    )
endif()

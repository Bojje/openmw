add_executable(openmw-vulkan-math-tests math_tests.cpp)

target_include_directories(openmw-vulkan-math-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if (BUILD_TESTING)
    add_test(NAME openmw-vulkan-math-tests COMMAND openmw-vulkan-math-tests)
endif()

add_executable(openmw-render-world-tests world_tests.cpp)

target_include_directories(openmw-render-world-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if (BUILD_TESTING)
    add_test(NAME openmw-render-world-tests COMMAND openmw-render-world-tests)
endif()

add_executable(openmw-render-mesh-tests mesh_tests.cpp)

target_include_directories(openmw-render-mesh-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(openmw-render-mesh-tests
    PRIVATE
    components
)

if (BUILD_TESTING)
    add_test(NAME openmw-render-mesh-tests COMMAND openmw-render-mesh-tests)
endif()

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
    set_tests_properties(openmw-vulkan-smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(openmw-vulkan-math-tests math_tests.cpp)

target_include_directories(openmw-vulkan-math-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if (BUILD_TESTING)
    add_test(NAME openmw-vulkan-math-tests COMMAND openmw-vulkan-math-tests)
endif()

add_executable(openmw-render-neutral-tests render_neutral_tests.cpp)

target_include_directories(openmw-render-neutral-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(openmw-render-neutral-tests
    PRIVATE
    openmw_render_neutral
)

if (BUILD_TESTING)
    add_test(NAME openmw-render-neutral-tests COMMAND openmw-render-neutral-tests)
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

add_executable(openmw-vulkan-image-tests image_tests.cpp)

target_include_directories(openmw-vulkan-image-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if (BUILD_TESTING)
    add_test(NAME openmw-vulkan-image-tests COMMAND openmw-vulkan-image-tests)
endif()

add_executable(openmw-resource-backend-tests resource_tests.cpp)

target_include_directories(openmw-resource-backend-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(openmw-resource-backend-tests
    PRIVATE
    components
)

if (BUILD_TESTING)
    add_test(NAME openmw-resource-backend-tests COMMAND openmw-resource-backend-tests)
endif()

add_executable(openmw-vulkan-terrain-tests terrain_tests.cpp)

target_include_directories(openmw-vulkan-terrain-tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(openmw-vulkan-terrain-tests
    PRIVATE
    components
)

if (BUILD_TESTING)
    add_test(NAME openmw-vulkan-terrain-tests COMMAND openmw-vulkan-terrain-tests)
endif()

add_executable(openmw-vulkan-smoke renderer_smoke.cpp)

target_link_libraries(openmw-vulkan-smoke
    PRIVATE
    openmw_vulkan
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

    add_test(
        NAME openmw-vulkan-headless-smoke
        COMMAND openmw-vulkan-smoke "${VK_SHADER_OUTPUT_DIR}" 3
    )
    set_tests_properties(openmw-vulkan-headless-smoke PROPERTIES
        ENVIRONMENT "OPENMW_VULKAN_HEADLESS=1"
        SKIP_RETURN_CODE 77
    )
endif()

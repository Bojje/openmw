function(compile_vulkan_shaders TARGET_NAME SHADER_DIR OUTPUT_DIR)
    file(GLOB RASTER_SHADERS
        "${SHADER_DIR}/*.vert"
        "${SHADER_DIR}/*.frag"
    )
    file(GLOB RT_SHADERS
        "${SHADER_DIR}/*.rgen"
        "${SHADER_DIR}/*.rchit"
        "${SHADER_DIR}/*.rmiss"
        "${SHADER_DIR}/*.rahit"
    )

    set(SPV_FILES)

    foreach(SHADER ${RASTER_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_FILE "${OUTPUT_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLANG_VALIDATOR} --target-env vulkan1.2 -V -o ${SPV_FILE} ${SHADER}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
        )
        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()

    foreach(SHADER ${RT_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_FILE "${OUTPUT_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLANG_VALIDATOR} --target-env spirv1.4 -V -o ${SPV_FILE} ${SHADER}
            DEPENDS ${SHADER}
            COMMENT "Compiling RT shader ${SHADER_NAME} to SPIR-V"
        )
        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SPV_FILES})
endfunction()

function(compile_vulkan_shaders TARGET_NAME SHADER_DIR OUTPUT_DIR)
    file(GLOB RASTER_SHADERS
        "${SHADER_DIR}/*.vert"
        "${SHADER_DIR}/*.frag"
    )
    set(SPV_FILES)

    foreach(SHADER ${RASTER_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_FILE "${OUTPUT_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND Vulkan::glslangValidator --target-env vulkan1.2 -V -o ${SPV_FILE} ${SHADER}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
        )
        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${SPV_FILES})
endfunction()

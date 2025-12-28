idf_component_register(SRCS ./main.cpp
                            ./disp_drv.cpp
                       INCLUDE_DIRS ./.. ./../components/lcddrv ./../components/snes9x ./../components/i2s_audio
                       REQUIRES
                       PRIV_REQUIRES )

target_compile_options(${COMPONENT_LIB} PRIVATE -DCOMPILEDATE="$(COMPILEDATE)" -DGITREV="$(GITREV)")

#set (CMAKE_CXX_FLAGS "")
#set (CMAKE_C_FLAGS "-mlongcalls")

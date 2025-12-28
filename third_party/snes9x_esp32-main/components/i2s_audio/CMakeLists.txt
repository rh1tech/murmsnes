idf_component_register(SRCS ./i2s_audio.cpp
                       INCLUDE_DIRS ./..
                       REQUIRES main esp_event driver
                       PRIV_REQUIRES )

set (CMAKE_CXX_FLAGS "-march=rv32imafc -mabi=ilp32f") #"-Wno-misleading-indentation -Wno-narrowing -Wno-format -mlongcalls -fpermissive")
set (CMAKE_C_FLAGS "-march=rv32imafc -mabi=ilp32f -Wno-format -Wno-unused-const-variable")

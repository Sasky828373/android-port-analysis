cmake_minimum_required(VERSION 3.22)
project(gtav_native_renderer LANGUAGES CXX)

set(DXBC_SPIRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxbc-spirv" CACHE PATH "dxbc-spirv source tree")
if(NOT EXISTS "${DXBC_SPIRV_DIR}/dxbc/dxbc_api.cpp")
  message(FATAL_ERROR "dxbc-spirv source missing at ${DXBC_SPIRV_DIR}")
endif()

file(GLOB DXBC_SPIRV_IR CONFIGURE_DEPENDS
  "${DXBC_SPIRV_DIR}/ir/*.cpp"
  "${DXBC_SPIRV_DIR}/ir/passes/*.cpp")
file(GLOB DXBC_SPIRV_DXBC CONFIGURE_DEPENDS "${DXBC_SPIRV_DIR}/dxbc/*.cpp")
file(GLOB DXBC_SPIRV_SPV CONFIGURE_DEPENDS "${DXBC_SPIRV_DIR}/spirv/*.cpp")
file(GLOB DXBC_SPIRV_UTIL CONFIGURE_DEPENDS "${DXBC_SPIRV_DIR}/util/*.cpp")

add_library(dxbc_spv STATIC
  ${DXBC_SPIRV_IR} ${DXBC_SPIRV_DXBC} ${DXBC_SPIRV_SPV} ${DXBC_SPIRV_UTIL})
target_compile_features(dxbc_spv PUBLIC cxx_std_17)
target_compile_definitions(dxbc_spv PUBLIC DXBC_SPV_ENABLE_SM5 DXBC_SPV_ENABLE_SPIRV)
target_include_directories(dxbc_spv PUBLIC "${DXBC_SPIRV_DIR}")
set_target_properties(dxbc_spv PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(gtav_native_renderer SHARED src/native_renderer.cpp)
target_compile_features(gtav_native_renderer PRIVATE cxx_std_20)
target_compile_options(gtav_native_renderer PRIVATE -O3 -DNDEBUG -flto=thin -fomit-frame-pointer -fvisibility=hidden -ffunction-sections -fdata-sections)
target_link_options(gtav_native_renderer PRIVATE -flto=thin -Wl,--gc-sections)
target_link_libraries(gtav_native_renderer PRIVATE dxbc_spv vulkan android log dl)

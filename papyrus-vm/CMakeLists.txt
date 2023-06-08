project(papyrus-vm VERSION 1.0.0)
include(${CMAKE_SOURCE_DIR}/cmake/apply_default_settings.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/link_vcpkg_dependencies.cmake)

file(GLOB src "${CMAKE_CURRENT_SOURCE_DIR}/src/papyrus-vm-lib/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/papyrus-vm-lib/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
list(APPEND src "${CMAKE_SOURCE_DIR}/.clang-format")
add_library(papyrus-vm-lib STATIC ${src})
apply_default_settings(TARGETS papyrus-vm-lib)
target_include_directories(papyrus-vm-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(papyrus-vm-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# TODO: fetch this via vcpkg
target_include_directories(papyrus-vm-lib PUBLIC "${CMAKE_SOURCE_DIR}/skymp5-server/cpp/third_party")

if(UNIX)
  target_link_libraries(papyrus-vm-lib PUBLIC stdc++fs)
endif()
target_link_libraries(papyrus-vm-lib PUBLIC viet)
link_vcpkg_dependencies(TARGETS papyrus-vm-lib)

file(GLOB src "${CMAKE_CURRENT_SOURCE_DIR}/src/papyrus-vm/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/papyrus-vm/*.cpp")
add_executable(papyrus-vm ${src})
target_link_libraries(papyrus-vm PUBLIC papyrus-vm-lib)
apply_default_settings(TARGETS papyrus-vm)
link_vcpkg_dependencies(TARGETS papyrus-vm)
add_custom_command(TARGET papyrus-vm POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:papyrus-vm> "${CMAKE_BINARY_DIR}/dist/papyrus-vm/$<TARGET_FILE_NAME:papyrus-vm>")
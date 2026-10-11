cmake_minimum_required(VERSION 3.15)
project(AudioCapture VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find packages
find_package(Opus CONFIG REQUIRED)
find_package(Ogg CONFIG REQUIRED)
find_package(FLAC CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/AudioCapture.cpp
    src/ProcessEnumerator.cpp
    src/WavWriter.cpp
    src/Mp3Encoder.cpp
    src/OpusEncoder.cpp
    src/FlacEncoder.cpp
    src/CaptureManager.cpp
    src/AudioDeviceEnumerator.cpp
    src/AudioMixer.cpp
    src/Updater.cpp
    resource.rc
)

set(HEADERS
    include/AudioCapture.h
    include/ProcessEnumerator.h
    include/WavWriter.h
    include/Mp3Encoder.h
    include/OpusEncoder.h
    include/FlacEncoder.h
    include/CaptureManager.h
    include/AudioDeviceEnumerator.h
    include/AudioMixer.h
    include/SystemDll.h
    include/Updater.h
    include/resource.h
)

# Build identity for the updater, regenerated on every build (see tools/generate_build_info.cmake)
set(BUILD_INFO_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(build_info ALL
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${BUILD_INFO_DIR}/build_info.h -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_build_info.cmake
    BYPRODUCTS ${BUILD_INFO_DIR}/build_info.h
    COMMENT "Recording the build commit for the updater")

# Create executable
add_executable(AudioCapture WIN32 ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(AudioCapture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${BUILD_INFO_DIR}
)
add_dependencies(AudioCapture build_info)

# Link Windows libraries and third-party libraries
target_link_libraries(AudioCapture PRIVATE
    Avrt.lib
    Mmdevapi.lib
    Ksuser.lib
    Mfplat.lib
    Mfreadwrite.lib
    Mfuuid.lib
    Ole32.lib
    Shell32.lib
    Comctl32.lib
    Shlwapi.lib
    Propsys.lib
    RuntimeObject.lib
    Delayimp.lib
    Opus::opus
    Ogg::ogg
    FLAC::FLAC FLAC::FLAC++
    nlohmann_json::nlohmann_json
)

# Enable Windows subsystem and static runtime
set_target_properties(AudioCapture PROPERTIES
    WIN32_EXECUTABLE TRUE
    LINK_FLAGS "/DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Add compile definitions
target_compile_definitions(AudioCapture PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Enable warnings
if(MSVC)
    target_compile_options(AudioCapture PRIVATE /W4)
else()
    target_compile_options(AudioCapture PRIVATE -Wall -Wextra)
endif()

# Unit checks for the updater's pure pieces (not shipped). See tools/updater_tests.cpp.
add_executable(updater_tests tools/updater_tests.cpp src/Updater.cpp)
target_include_directories(updater_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${BUILD_INFO_DIR})
target_link_libraries(updater_tests PRIVATE nlohmann_json::nlohmann_json Bcrypt.lib)
target_compile_definitions(updater_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(updater_tests PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(updater_tests build_info)

# Prints the rows the process list would show (not shipped). See tools/process_list_check.cpp.
add_executable(process_list_check tools/process_list_check.cpp src/ProcessEnumerator.cpp)
target_include_directories(process_list_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(process_list_check PRIVATE Ole32.lib)
target_compile_definitions(process_list_check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(process_list_check PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Console harness for the capture path (not shipped; build.bat and CI build only AudioCapture).
# See tools/capture_smoke.cpp for usage.
add_executable(capture_smoke tools/capture_smoke.cpp src/AudioCapture.cpp src/WavWriter.cpp)
target_include_directories(capture_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(capture_smoke PRIVATE Avrt.lib Mmdevapi.lib Ole32.lib)
target_compile_definitions(capture_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(capture_smoke PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

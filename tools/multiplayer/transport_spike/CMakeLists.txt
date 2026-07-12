cmake_minimum_required(VERSION 3.22)

project(cdda_multiplayer_transport_spike LANGUAGES CXX)

include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(MULTIPLAYER_ASIO_VERSION "1.38.1" CACHE STRING "Pinned standalone Asio version")
set(MULTIPLAYER_ASIO_TAG "asio-1-38-1" CACHE STRING "Pinned standalone Asio tag")
set(MULTIPLAYER_ASIO_SHA256
    "2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc"
    CACHE STRING "SHA-256 of the pinned standalone Asio source archive")

set(asio_url
    "https://github.com/chriskohlhoff/asio/archive/refs/tags/${MULTIPLAYER_ASIO_TAG}.tar.gz")
FetchContent_Declare(
    multiplayer_asio
    URL "${asio_url}"
    URL_HASH "SHA256=${MULTIPLAYER_ASIO_SHA256}"
)
FetchContent_MakeAvailable(multiplayer_asio)

add_executable(multiplayer_transport_spike transport_spike.cpp)
target_compile_features(multiplayer_transport_spike PRIVATE cxx_std_17)
target_compile_definitions(
    multiplayer_transport_spike
    PRIVATE
        ASIO_NO_DEPRECATED
        ASIO_STANDALONE
)
target_include_directories(
    multiplayer_transport_spike
    SYSTEM PRIVATE "${multiplayer_asio_SOURCE_DIR}/include"
)

if(MSVC)
    target_compile_definitions(
        multiplayer_transport_spike
        PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0A00
    )
    target_compile_options(multiplayer_transport_spike PRIVATE /permissive- /W4 /WX)
    target_link_libraries(multiplayer_transport_spike PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_compile_options(multiplayer_transport_spike PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(multiplayer_transport_spike PRIVATE Threads::Threads)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_test(NAME multiplayer_transport_loopback COMMAND multiplayer_transport_spike)
    set_tests_properties(multiplayer_transport_loopback PROPERTIES TIMEOUT 30)
endif()

message(STATUS "Multiplayer transport spike uses standalone Asio ${MULTIPLAYER_ASIO_VERSION}")
message(STATUS "Standalone Asio archive SHA-256: ${MULTIPLAYER_ASIO_SHA256}")

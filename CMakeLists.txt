cmake_minimum_required(VERSION 3.16)
project(NexonMiniGameServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(nexon_mini_server
    src/main.cpp
    src/GameServer.cpp
    src/Logger.cpp
)

add_executable(nexon_mini_client
    src/GameClient.cpp
)

add_executable(nexon_mini_client_gui WIN32
    src/GameClientGui.cpp
)

target_include_directories(nexon_mini_server PRIVATE include)
target_include_directories(nexon_mini_client PRIVATE include)
target_include_directories(nexon_mini_client_gui PRIVATE include)

target_compile_definitions(nexon_mini_server PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(nexon_mini_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(nexon_mini_client_gui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if (MINGW)
    target_link_options(nexon_mini_server PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(nexon_mini_client PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(nexon_mini_client_gui PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(nexon_mini_server PRIVATE ws2_32)
target_link_libraries(nexon_mini_client PRIVATE ws2_32)
target_link_libraries(nexon_mini_client_gui PRIVATE ws2_32 user32 gdi32)

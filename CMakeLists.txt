cmake_minimum_required(VERSION 3.20)
project(SectorFlux VERSION 1.0.0 LANGUAGES CXX C)

# Generate version header from CMake version
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Asio (Standalone)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)
set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)

# Crow (Web Framework)
FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.3.0
)
FetchContent_MakeAvailable(Crow)

# nlohmann/json (JSON Parser)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# cpp-httplib (HTTP Client)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

# SQLite3 (skip the repo's outdated CMakeLists.txt by pointing to non-existent subdir)
FetchContent_Declare(
    sqlite3
    GIT_REPOSITORY https://github.com/azadkuh/sqlite-amalgamation.git
    GIT_TAG master
    SOURCE_SUBDIR _skip_cmake
)
FetchContent_MakeAvailable(sqlite3)

# Create our own sqlite3 library target
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
set_target_properties(sqlite3 PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Generate embedded UI header from public/ files
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_ui.hpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_ui.py
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_ui.py
            ${CMAKE_CURRENT_SOURCE_DIR}/public/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/public/app.js
            ${CMAKE_CURRENT_SOURCE_DIR}/public/api.js
            ${CMAKE_CURRENT_SOURCE_DIR}/public/style.css
    COMMENT "Embedding UI files into C++ header"
)

add_executable(SectorFlux
    src/main.cpp
    src/proxy.cpp
    src/database.cpp
    src/embedded_ui.hpp
)

# Mark ASIO includes as SYSTEM to suppress warnings
target_include_directories(SectorFlux PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(SectorFlux SYSTEM PRIVATE ${ASIO_INCLUDE_DIR})

target_link_libraries(SectorFlux PRIVATE
    Crow::Crow
    nlohmann_json::nlohmann_json
    httplib::httplib
    sqlite3
)

if(MSVC)
    # Compiler options for our code
    target_compile_options(SectorFlux PRIVATE
        /W4                        # High warning level for our code
        /external:anglebrackets    # Treat #include <...> as external
        /external:W0               # No warnings for external headers
    )
    # Define Windows version for Asio (Windows 10)
    target_compile_definitions(SectorFlux PRIVATE _WIN32_WINNT=0x0A00)
else()
    target_compile_options(SectorFlux PRIVATE -Wall -Wextra)
endif()

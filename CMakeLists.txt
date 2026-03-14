cmake_minimum_required(VERSION 3.10)
project(c-erp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# Sources
# ============================================================
file(GLOB SOURCES "*.cpp")

add_executable(c-erp ${SOURCES})

# ============================================================
# Third-party: Drogon + trantor (bundled)
# ============================================================
add_subdirectory(3rdparty/json)
add_subdirectory(3rdparty/drogon)

# ============================================================
# Third-party: libpqxx (bundled — built from source)
# ============================================================
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_DOC  OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/libpqxx)

# ============================================================
# Include directories
# ============================================================
target_include_directories(c-erp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/factories
    ${CMAKE_CURRENT_SOURCE_DIR}/core/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/base
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/lib/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libpqxx/include
)

# ============================================================
# Link
# ============================================================
target_link_libraries(c-erp PRIVATE
    drogon   # HTTP + WebSocket (pulls in trantor)
    pqxx     # PostgreSQL C++ client (built from 3rdparty/libpqxx)
)

message(STATUS "Source files: ${SOURCES}")

# cmake_minimum_required(VERSION 3.10)
# project(c-erp)

# # Use GLOB to find all .cpp files in the current directory
# # and store them in the SOURCES variable
# file(GLOB SOURCES "*.cpp")

# # Add the executable using the generated list of files
# add_executable(c-erp ${SOURCES})

# add_subdirectory(3rdparty/json)
# add_subdirectory(3rdparty/drogon)

# # Add include directories for the project
# target_include_directories(c-erp PRIVATE
#     ${CMAKE_CURRENT_SOURCE_DIR}/core
#     ${CMAKE_CURRENT_SOURCE_DIR}/core/factories
#     ${CMAKE_CURRENT_SOURCE_DIR}/core/interfaces
#     ${CMAKE_CURRENT_SOURCE_DIR}/core/services
#     ${CMAKE_CURRENT_SOURCE_DIR}/modules/base
#     ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
#     ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/lib/inc
#     ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/
# )

# target_link_libraries(c-erp
#     PRIVATE
#     drogon
#     pqxx
# )
# # Print the list of files found
# message(STATUS "Source Files: ${SOURCES}")
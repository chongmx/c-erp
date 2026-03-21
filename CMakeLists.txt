cmake_minimum_required(VERSION 3.10)
project(c-erp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources
file(GLOB SOURCES "*.cpp")
add_executable(c-erp ${SOURCES})

# Third-party
add_subdirectory(3rdparty/json)
add_subdirectory(3rdparty/drogon)
set(BUILD_TEST     OFF CACHE BOOL "" FORCE)
set(BUILD_DOC      OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/libpqxx)

# Include directories
target_include_directories(c-erp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/factories
    ${CMAKE_CURRENT_SOURCE_DIR}/core/infrastructure
    ${CMAKE_CURRENT_SOURCE_DIR}/core/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/base
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/auth
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/uom
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/product
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/sale
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/purchase
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/hr
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/stock
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/mail
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/report
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/lib/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libpqxx/include
)

# Link
target_link_libraries(c-erp PRIVATE
    drogon
    pqxx
)

message(STATUS "Source files: ${SOURCES}")

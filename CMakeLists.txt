cmake_minimum_required(VERSION 3.10)
project(c-erp)

# Use GLOB to find all .cpp files in the current directory
# and store them in the SOURCES variable
file(GLOB SOURCES "*.cpp")

# Add the executable using the generated list of files
add_executable(c-erp ${SOURCES})

add_subdirectory(3rdparty/json)
add_subdirectory(3rdparty/drogon)

# Add include directories for the project
target_include_directories(c-erp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/factories
    ${CMAKE_CURRENT_SOURCE_DIR}/core/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/core/services
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/base
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/lib/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/trantor
)

# target_link_libraries(c_erp
#     PRIVATE
#     drogon
# )
# Print the list of files found
message(STATUS "Source Files: ${SOURCES}")
cmake_minimum_required(VERSION 3.10)
project(c-erp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources
file(GLOB SOURCES "*.cpp")
file(GLOB_RECURSE MODULE_SOURCES
    "core/*.cpp"
    "modules/*.cpp"
    "factories/*.cpp"
)
add_executable(c-erp ${SOURCES} ${MODULE_SOURCES})

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
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/ir
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/auth
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/uom
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/product
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/sale
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/purchase
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/hr
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/stock
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/mail
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/report
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/portal
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/rental
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

# ------------------------------------------------------------------
# P7 — unit tests (docs/033 §3)
#
#   cmake --build ./build --target erp_tests && ./build/erp_tests
#
# Deliberately NOT built by the default target: `cmake --build ./build`
# stays the fast path for iterating on the server. scripts/run_tests.sh
# builds it explicitly and then runs the integration suites too.
#
# Only the units under test are compiled in. Linking the whole module set
# would drag in Container and every module's static registration, which
# would need a live database to construct — the point of these cases is
# that they run with no database at all.
# ------------------------------------------------------------------
file(GLOB TEST_SOURCES "tests/*.cpp")
add_executable(erp_tests EXCLUDE_FROM_ALL
    ${TEST_SOURCES}
    core/Money.cpp
    core/TaxEngine.cpp
    # SessionManager is header-only; test_session.cpp includes it directly.
)
target_include_directories(erp_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/infrastructure
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
)
target_link_libraries(erp_tests PRIVATE ssl crypto)

# ------------------------------------------------------------------
# ERP Admin Console (docs/073) — a SEPARATE, loopback-only IT admin tool.
#
#   cmake --build ./build --target erp-admin && ./build/erp-admin
#
# EXCLUDE_FROM_ALL so it never slows the default server build. It reuses the
# vendored Drogon for its tiny HTTP server and shells out to system tools
# (psql/pg_dump/nginx/certbot/systemctl) — no libpqxx dependency.
# ------------------------------------------------------------------
add_executable(erp-admin EXCLUDE_FROM_ALL tools/admin/erp_admin.cpp)
target_include_directories(erp-admin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/lib/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/drogon/trantor
)
target_link_libraries(erp-admin PRIVATE drogon)

message(STATUS "Source files: ${SOURCES}")

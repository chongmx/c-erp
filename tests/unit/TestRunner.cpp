// ============================================================
// tests/unit/TestRunner.cpp — the single main() for erp_tests (P7)
//
//   ./build/erp_tests            run everything
//   ./build/erp_tests Money      run only cases whose name contains "Money"
//
// Every other file under tests/ contains ERP_TEST cases and no main().
// ============================================================
#include "TestHarness.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string filter = (argc > 1) ? argv[1] : "";

    std::printf("============================================\n");
    std::printf("  c-erp unit tests\n");
    if (!filter.empty()) std::printf("  filter: \"%s\"\n", filter.c_str());
    std::printf("============================================\n");

    return ::erptest::TestRegistry::instance().runAll(filter) > 0 ? 1 : 0;
}

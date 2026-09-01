// =============================================================
// Negative control for S-39.
//
// Reproduces the PRE-FIX construction verbatim (std::system with the
// request-derived id interpolated into the path). If this creates the
// marker file, the payload used by verify_security_fixes.sh is live —
// which is what makes "no marker file appeared" against the fixed build
// meaningful rather than vacuous.
//
// Build & run from the repo root:
//   g++ -o /tmp/negctl scripts/negctl_s39.cpp && /tmp/negctl ; ls negctl_PWNED
// =============================================================
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    const std::string model = "sale.order";

    // Exactly what the URL segment "2$(touch negctl_PWNED)" would deliver.
    // Assembled from parts so no editor/shell can expand it in transit.
    const std::string dollar = "$";
    const std::string idStr  = "2" + dollar + "(touch negctl_PWNED)";

    // ---- pre-fix code, verbatim ----
    const std::string tmpBase = "/tmp/erp_report_" + model + "_" + idStr;
    const std::string tmpHtml = tmpBase + ".html";
    const std::string tmpPdf  = tmpBase + ".pdf";
    const std::string cmd = std::string("wkhtmltopdf --quiet")
        + " --enable-local-file-access"
        + " \"" + tmpHtml + "\""
        + " \"" + tmpPdf  + "\""
        + " 2>/dev/null";
    // --------------------------------

    std::printf("cmd: %s\n", cmd.c_str());
    const int rc = std::system(cmd.c_str());
    std::printf("wkhtmltopdf rc=%d (non-zero is expected and irrelevant —\n"
                "  the command substitution runs during shell parsing,\n"
                "  before wkhtmltopdf is even invoked)\n", rc);
    return 0;
}

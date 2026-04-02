#pragma once
// =============================================================
// core/infrastructure/CsvParser.hpp  — header-only RFC 4180 CSV parser
// =============================================================
// No external dependencies — pure string manipulation.
//
// parseCsv: handles quoted fields, "" escape sequences, CRLF and LF line endings.
// buildCsv: quotes fields that contain commas, double-quotes, or newlines.
// =============================================================
#include <string>
#include <vector>

namespace odoo::infrastructure {

/**
 * @brief Parse a CSV string into a 2-D vector of strings.
 *
 * Conforms to RFC 4180:
 *   - Fields may be quoted with double-quotes.
 *   - A double-quote inside a quoted field is escaped as "".
 *   - Both CRLF and LF are accepted as line endings.
 *   - A trailing newline on the last record is silently ignored.
 *
 * @param text  Raw CSV content (any line ending).
 * @returns     Vector of rows; each row is a vector of field strings.
 *              Empty input returns an empty vector.
 */
inline std::vector<std::vector<std::string>> parseCsv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    if (text.empty()) return rows;

    std::vector<std::string> row;
    std::string field;
    bool inQuote = false;
    std::size_t i = 0;
    const std::size_t n = text.size();

    while (i < n) {
        const char c = text[i];

        if (inQuote) {
            if (c == '"') {
                // Peek: "" → escaped quote; single " → end of quoted field
                if (i + 1 < n && text[i + 1] == '"') {
                    field += '"';
                    i += 2;
                } else {
                    inQuote = false;
                    ++i;
                }
            } else {
                // Absorb CRLF inside quotes as LF only
                if (c == '\r' && i + 1 < n && text[i + 1] == '\n') {
                    field += '\n';
                    i += 2;
                } else {
                    field += c;
                    ++i;
                }
            }
        } else {
            if (c == '"') {
                inQuote = true;
                ++i;
            } else if (c == ',') {
                row.push_back(std::move(field));
                field.clear();
                ++i;
            } else if (c == '\n' || (c == '\r' && i + 1 < n && text[i + 1] == '\n')) {
                // End of record
                row.push_back(std::move(field));
                field.clear();
                rows.push_back(std::move(row));
                row.clear();
                i += (c == '\r') ? 2 : 1;
            } else if (c == '\r') {
                // Bare CR treated as record separator (non-standard but common)
                row.push_back(std::move(field));
                field.clear();
                rows.push_back(std::move(row));
                row.clear();
                ++i;
            } else {
                field += c;
                ++i;
            }
        }
    }

    // Flush final field/record (file without trailing newline)
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }

    return rows;
}

/**
 * @brief Serialise a 2-D vector of strings to an RFC 4180 CSV string.
 *
 * Fields are quoted if they contain commas, double-quotes, newlines, or
 * carriage returns.  Double-quotes inside fields are escaped as "".
 * Records are terminated with CRLF per RFC 4180.
 *
 * @param rows  Data to serialise.
 * @returns     CSV string with CRLF line endings.
 */
inline std::string buildCsv(const std::vector<std::vector<std::string>>& rows) {
    std::string out;
    out.reserve(rows.size() * 64);  // rough pre-allocation

    for (const auto& row : rows) {
        bool firstField = true;
        for (const auto& field : row) {
            if (!firstField) out += ',';
            firstField = false;

            // Quote if field contains special characters
            const bool needsQuote = field.find_first_of(",\"\r\n") != std::string::npos;
            if (needsQuote) {
                out += '"';
                for (const char c : field) {
                    if (c == '"') out += '"';  // escape double-quote
                    out += c;
                }
                out += '"';
            } else {
                out += field;
            }
        }
        out += "\r\n";
    }
    return out;
}

} // namespace odoo::infrastructure

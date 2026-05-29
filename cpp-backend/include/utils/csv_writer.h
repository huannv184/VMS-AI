#pragma once

#include <string>

namespace vms {
namespace utils {

// RFC 4180 + Excel formula-injection safe field escape.
//
// Pre-fix the reporting controller wrote CSV by string concatenation with
// no escaping, so:
//   * a `description` containing a comma broke column alignment,
//   * a `description` containing `"` broke even the quoted column,
//   * a `description` starting with `=`, `+`, `-`, `@` was evaluated as
//     a formula when the operator opened the file in Excel/LibreOffice
//     (CSV injection — CVE-like UX-level remote code execution within
//     the spreadsheet context, e.g. `=cmd|'/c calc'!A1`).
//
// Rules applied here:
//   1. If the field starts with `=`, `+`, `-`, `@`, `\t`, or `\r`, prefix
//      a single quote so Excel treats it as text, not a formula.
//   2. If the field contains `"`, `,`, `\n`, or `\r`, double-quote the
//      whole field and escape internal `"` as `""`.
//   3. Otherwise return the field as-is.
//
// The result is always safe to concatenate with `,` and `\n` separators.
inline std::string escapeCsvField(const std::string& in) {
    if (in.empty()) return in;

    std::string out;
    out.reserve(in.size() + 4);

    // Formula-injection guard: prefix a single quote if the first
    // character is one of Excel's formula triggers. The leading quote
    // disappears in the rendered cell but neutralizes the formula.
    const char first = in.front();
    const bool needs_formula_guard =
        first == '=' || first == '+' || first == '-' || first == '@' ||
        first == '\t' || first == '\r';
    if (needs_formula_guard) {
        out.push_back('\'');
    }

    bool needs_quote = false;
    for (char c : in) {
        if (c == '"') { out.push_back('"'); out.push_back('"'); needs_quote = true; }
        else if (c == ',' || c == '\n' || c == '\r') { out.push_back(c); needs_quote = true; }
        else out.push_back(c);
    }

    if (needs_quote || needs_formula_guard) {
        // Wrap in quotes. Formula-guard alone (no special chars inside)
        // still benefits from quoting because the leading `'` could be
        // misinterpreted by some parsers — quoting makes the boundary
        // explicit.
        return std::string("\"") + out + "\"";
    }
    return out;
}

} // namespace utils
} // namespace vms

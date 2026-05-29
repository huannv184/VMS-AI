// CSV escape unit tests. Header-only — no link against backend code
// because csv_writer.h is self-contained.
//
// Regression target: pre-fix reporting_controller wrote CSV by raw
// string concat. Description/direction/video_path containing a `,` or
// `"` broke the CSV; description starting with `=` / `+` / `-` / `@`
// executed as a formula when the operator opened the export in Excel.
//
// These tests pin the contract that escapeCsvField() must:
//   1. Pass safe ASCII through unchanged.
//   2. Quote and double up internal `"` characters.
//   3. Quote fields containing `,`, `\n`, `\r`.
//   4. Prefix `'` to neutralize formula triggers.
//   5. Combine guarding + quoting for compound cases.

#include "utils/csv_writer.h"
#include <gtest/gtest.h>

using vms::utils::escapeCsvField;

TEST(CsvEscape, SafeAsciiPassesThrough) {
    EXPECT_EQ(escapeCsvField("hello"), "hello");
    EXPECT_EQ(escapeCsvField("person"), "person");
    EXPECT_EQ(escapeCsvField("ABC123"), "ABC123");
}

TEST(CsvEscape, EmptyStringPreserved) {
    EXPECT_EQ(escapeCsvField(""), "");
}

TEST(CsvEscape, CommaForcesQuoting) {
    EXPECT_EQ(escapeCsvField("a,b"), "\"a,b\"");
    EXPECT_EQ(escapeCsvField("one, two, three"), "\"one, two, three\"");
}

TEST(CsvEscape, DoubleQuoteEscapedAndWrapped) {
    EXPECT_EQ(escapeCsvField("he said \"hi\""), "\"he said \"\"hi\"\"\"");
    EXPECT_EQ(escapeCsvField("\""), "\"\"\"\"");
}

TEST(CsvEscape, NewlineForcesQuoting) {
    EXPECT_EQ(escapeCsvField("line1\nline2"), "\"line1\nline2\"");
    EXPECT_EQ(escapeCsvField("a\r\nb"), "\"a\r\nb\"");
}

TEST(CsvEscape, FormulaPrefixGuardEquals) {
    // =cmd|'/c calc'!A1 — classic Excel RCE payload. Must be prefixed
    // with `'` so Excel treats it as text. We also wrap because the
    // leading quote needs to render reliably.
    const std::string evil = "=cmd|'/c calc'!A1";
    const std::string result = escapeCsvField(evil);
    EXPECT_EQ(result.front(), '"') << "should be wrapped in quotes";
    // The `'` neutralizer must appear immediately after the opening quote.
    EXPECT_EQ(result.substr(0, 2), "\"'") << "should start with quote+apostrophe";
    // Original `=` must still be present (just neutralized, not stripped).
    EXPECT_NE(result.find('='), std::string::npos);
}

TEST(CsvEscape, FormulaPrefixGuardPlus) {
    const std::string r = escapeCsvField("+1234");
    EXPECT_EQ(r.substr(0, 2), "\"'");
}

TEST(CsvEscape, FormulaPrefixGuardMinus) {
    const std::string r = escapeCsvField("-1234");
    EXPECT_EQ(r.substr(0, 2), "\"'");
}

TEST(CsvEscape, FormulaPrefixGuardAt) {
    const std::string r = escapeCsvField("@SUM(A1:A10)");
    EXPECT_EQ(r.substr(0, 2), "\"'");
}

TEST(CsvEscape, FormulaPrefixGuardTab) {
    // Tab and CR are documented formula-trigger characters in some
    // spreadsheet contexts (precursor to formula on the next line).
    const std::string r = escapeCsvField("\thello");
    EXPECT_EQ(r.substr(0, 2), "\"'");
}

TEST(CsvEscape, NonFormulaWithSpecialCharCombined) {
    // Field starts with `=` AND contains `,` — must be quoted, internal
    // chars preserved, and the formula-guard `'` injected.
    const std::string in = "=A1,B1";
    const std::string r = escapeCsvField(in);
    ASSERT_EQ(r.front(), '"');
    ASSERT_EQ(r.back(), '"');
    EXPECT_EQ(r.find("'="), 1u) << "guard before original formula char";
    EXPECT_NE(r.find(','), std::string::npos) << "comma preserved";
}

TEST(CsvEscape, RealWorldVietnameseDescription) {
    // Operators often write descriptions with commas + quoted brand names.
    const std::string in = "Vào khu vực \"hạn chế\", camera 3";
    const std::string r = escapeCsvField(in);
    // Should be wrapped and inner quotes doubled.
    EXPECT_EQ(r.front(), '"');
    EXPECT_EQ(r.back(), '"');
    EXPECT_NE(r.find("\"\"hạn chế\"\""), std::string::npos);
}

TEST(CsvEscape, VideoPathWithPathSeparators) {
    // video_path is filesystem-controlled; should pass through cleanly.
    EXPECT_EQ(escapeCsvField("recordings/cam_2/seg_0042.mp4"),
              "recordings/cam_2/seg_0042.mp4");
    // Windows path with backslashes is still safe.
    EXPECT_EQ(escapeCsvField("D:\\videos\\seg.mp4"), "D:\\videos\\seg.mp4");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "utils/api_utils.h"

using json = nlohmann::json;

TEST(ApiUtilsSafeError, RedactsExceptionTextFromClientResponse) {
    const std::runtime_error e("SQLITE_ERROR: no such column: secret_token");
    auto res = vms::api::ApiUtils::createSafeError(e, 500, "https://frontend.example", "unit-test");

    EXPECT_EQ(res.code, 500);
    EXPECT_EQ(res.get_header_value("Access-Control-Allow-Origin"), "https://frontend.example");

    const auto body = json::parse(res.body);
    ASSERT_TRUE(body.contains("error"));
    ASSERT_TRUE(body["error"].is_object());
    EXPECT_EQ(body["error"]["message"], "Internal server error");
    EXPECT_EQ(body["error"]["code"], "INTERNAL_ERROR");
    EXPECT_EQ(res.body.find("secret_token"), std::string::npos);
    EXPECT_EQ(res.body.find("no such column"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

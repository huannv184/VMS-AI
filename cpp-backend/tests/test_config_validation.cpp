#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "utils/config.h"

namespace {

std::filesystem::path writeTempYaml(const std::string& stem, const std::string& body) {
    const auto dir = std::filesystem::temp_directory_path() / "vms_config_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / (stem + ".yaml");
    std::ofstream out(path);
    out << body;
    out.close();
    return path;
}

void clearRelevantEnv() {
#ifdef _WIN32
    _putenv_s("VMS_JWT_SECRET", "");
    _putenv_s("VMS_ALLOW_DEFAULT_SECRET", "");
    _putenv_s("VMS_MINIO_ACCESS_KEY", "");
    _putenv_s("VMS_MINIO_SECRET_KEY", "");
    _putenv_s("VMS_ALLOW_DEFAULT_MINIO_SECRET", "");
    _putenv_s("VMS_ALLOW_WILDCARD_CORS", "");
#else
    unsetenv("VMS_JWT_SECRET");
    unsetenv("VMS_ALLOW_DEFAULT_SECRET");
    unsetenv("VMS_MINIO_ACCESS_KEY");
    unsetenv("VMS_MINIO_SECRET_KEY");
    unsetenv("VMS_ALLOW_DEFAULT_MINIO_SECRET");
    unsetenv("VMS_ALLOW_WILDCARD_CORS");
#endif
}

} // namespace

class ConfigValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        clearRelevantEnv();
    }
};

TEST_F(ConfigValidationTest, AppliesLegacyFlatDatabaseKeys) {
    const auto path = writeTempYaml(
        "legacy_db_keys",
        R"(server:
  host: "127.0.0.1"
  port: 8000
auth:
  enabled: false
cors:
  enabled: false
database:
  driver: "sqlite"
  path: "data/events.db"
  connection_pool_size: 42
  busy_timeout_ms: 1234
)");

    ASSERT_TRUE(vms::Config::getInstance().loadFromFile(path.string()));
    const auto& db = vms::Config::getInstance().getDatabaseConfig();
    EXPECT_EQ(db.path, "data/events.db");
    EXPECT_EQ(db.sqlite.path, "data/events.db");
    EXPECT_EQ(db.sqlite.busy_timeout_ms, 1234);
    EXPECT_EQ(db.postgres.pool_size, 42);
}

TEST_F(ConfigValidationTest, RejectsWildcardCorsWhenCredentialsEnabled) {
    const auto path = writeTempYaml(
        "wildcard_cors",
        R"(server:
  host: "127.0.0.1"
  port: 8000
auth:
  enabled: false
cors:
  enabled: true
  origins: ["*"]
  methods: ["GET"]
  headers: ["Content-Type"]
  credentials: true
)");

    EXPECT_FALSE(vms::Config::getInstance().loadFromFile(path.string()));
}

TEST_F(ConfigValidationTest, RejectsDefaultMinioCredentialsWhenDriverEnabled) {
    const auto path = writeTempYaml(
        "minio_defaults",
        R"(server:
  host: "127.0.0.1"
  port: 8000
auth:
  enabled: false
cors:
  enabled: false
storage:
  driver: "minio"
  minio:
    endpoint: "http://localhost:9000"
    bucket_recordings: "vms-recordings"
    bucket_snapshots: "vms-snapshots"
)");

    EXPECT_FALSE(vms::Config::getInstance().loadFromFile(path.string()));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

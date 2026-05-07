#include <gtest/gtest.h>
#include <string>

// ── Inline reproduction of DbManager dialect helpers ─────────────────────────
// Tests the SQL expression selection logic without a live DB connection.

static std::string sqlNowEpoch(bool isPostgres) {
    return isPostgres ? "EXTRACT(EPOCH FROM NOW())::BIGINT" : "strftime('%s','now')";
}

static std::string buildUpsertPermissions(bool isPostgres) {
    return isPostgres
        ? "INSERT INTO permissions (user_id, allowed_cameras, allowed_sites, "
          "can_view_live, can_view_playback, can_manage_settings, can_ptz) "
          "VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT (user_id) DO UPDATE SET "
          "allowed_cameras=EXCLUDED.allowed_cameras, allowed_sites=EXCLUDED.allowed_sites, "
          "can_view_live=EXCLUDED.can_view_live, can_view_playback=EXCLUDED.can_view_playback, "
          "can_manage_settings=EXCLUDED.can_manage_settings, can_ptz=EXCLUDED.can_ptz"
        : "INSERT OR REPLACE INTO permissions (user_id, allowed_cameras, allowed_sites, "
          "can_view_live, can_view_playback, can_manage_settings, can_ptz) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)";
}

// ── sqlNowEpoch ───────────────────────────────────────────────────────────────
TEST(SqlDialect, NowEpochSQLite) {
    EXPECT_EQ(sqlNowEpoch(false), "strftime('%s','now')");
}

TEST(SqlDialect, NowEpochPostgres) {
    EXPECT_EQ(sqlNowEpoch(true), "EXTRACT(EPOCH FROM NOW())::BIGINT");
}

TEST(SqlDialect, NowEpochNoSQLiteInPostgresQuery) {
    auto q = sqlNowEpoch(true);
    EXPECT_EQ(q.find("strftime"), std::string::npos);
}

TEST(SqlDialect, NowEpochNoPostgresInSQLiteQuery) {
    auto q = sqlNowEpoch(false);
    EXPECT_EQ(q.find("EXTRACT"), std::string::npos);
}

// ── upsert permissions ────────────────────────────────────────────────────────
TEST(SqlDialect, PermissionsUpsertSQLiteUsesInsertOrReplace) {
    auto q = buildUpsertPermissions(false);
    EXPECT_NE(q.find("INSERT OR REPLACE"), std::string::npos);
}

TEST(SqlDialect, PermissionsUpsertPostgresUsesOnConflict) {
    auto q = buildUpsertPermissions(true);
    EXPECT_NE(q.find("ON CONFLICT"), std::string::npos);
    EXPECT_NE(q.find("DO UPDATE SET"), std::string::npos);
}

TEST(SqlDialect, PermissionsUpsertPostgresHasNoInsertOrReplace) {
    auto q = buildUpsertPermissions(true);
    EXPECT_EQ(q.find("INSERT OR REPLACE"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

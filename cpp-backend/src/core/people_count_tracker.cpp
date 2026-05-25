// ==============================================================
// File: src/core/people_count_tracker.cpp
// ==============================================================

#include "core/people_count_tracker.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "database/db_manager.h"
#include "utils/logger.h"

namespace vms::core {

namespace {

int readCooldownMsFromEnv() {
    const char* e = std::getenv("VMS_COUNTER_CROSSING_COOLDOWN_MS");
    if (!e || !*e) return 3000;
    int v = std::atoi(e);
    if (v < 100)        v = 100;
    if (v > 60 * 1000)  v = 60 * 1000;
    return v;
}

} // namespace

PeopleCountTracker& PeopleCountTracker::getInstance() {
    static PeopleCountTracker inst;
    return inst;
}

PeopleCountTracker::PeopleCountTracker()
    : cooldown_ms_(readCooldownMsFromEnv()) {}

void PeopleCountTracker::ensureLoaded() {
    if (loaded_.load(std::memory_order_acquire)) return;
    reloadFromDb();
}

void PeopleCountTracker::reloadFromDb() {
    std::unordered_map<int, std::vector<CountingLine>> next;
    try {
        auto db = vms::database::DbManager::getInstance().getThreadConnection();
        if (!db.isValid() || !db.isOpen()) {
            LOG_WARN("PeopleCountTracker: reloadFromDb skipped (DB unavailable)");
            return;
        }
        QSqlQuery q(db);
        if (!q.exec(
                "SELECT id, camera_id, name, ax, ay, bx, by, "
                "       direction_a_label, direction_b_label, "
                "       object_classes_json, enabled "
                "FROM counting_lines")) {
            LOG_WARN("PeopleCountTracker: counting_lines query failed: {}",
                     q.lastError().text().toStdString());
            return;
        }

        while (q.next()) {
            CountingLine ln;
            ln.id          = q.value(0).toInt();
            ln.camera_id   = q.value(1).toInt();
            ln.name        = q.value(2).toString().toStdString();
            ln.ax          = q.value(3).toFloat();
            ln.ay          = q.value(4).toFloat();
            ln.bx          = q.value(5).toFloat();
            ln.by          = q.value(6).toFloat();
            ln.dir_a_label = q.value(7).isNull() ? "in"  : q.value(7).toString().toStdString();
            ln.dir_b_label = q.value(8).isNull() ? "out" : q.value(8).toString().toStdString();
            ln.enabled     = q.value(10).toInt() == 1;

            const std::string raw = q.value(9).isNull() ? std::string{}
                                                        : q.value(9).toString().toStdString();
            if (!raw.empty()) {
                try {
                    auto j = nlohmann::json::parse(raw);
                    if (j.is_array()) {
                        for (const auto& item : j) {
                            if (item.is_string()) ln.object_classes.push_back(item.get<std::string>());
                        }
                    }
                } catch (...) {
                    // tolerate malformed config — treat as match-all
                }
            }
            next[ln.camera_id].push_back(std::move(ln));
        }
    } catch (const std::exception& e) {
        LOG_WARN("PeopleCountTracker: reloadFromDb exception: {}", e.what());
        return;
    }

    std::size_t total = 0;
    for (auto& [cam, vec] : next) total += vec.size();

    {
        std::unique_lock<std::shared_mutex> wr(cache_mu_);
        by_camera_.swap(next);
    }
    loaded_.store(true, std::memory_order_release);
    LOG_INFO("PeopleCountTracker: cache reloaded ({} lines)", total);
}

float PeopleCountTracker::signedSide(float px, float py,
                                     float ax, float ay,
                                     float bx, float by) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

bool PeopleCountTracker::segmentsIntersect(float p1x, float p1y,
                                           float p2x, float p2y,
                                           float ax, float ay,
                                           float bx, float by) {
    // Two segments (P1-P2) and (A-B) intersect iff each endpoint of one
    // segment is on opposite sides of the other.
    const float d1 = signedSide(p1x, p1y, ax, ay, bx, by);
    const float d2 = signedSide(p2x, p2y, ax, ay, bx, by);
    const float d3 = signedSide(ax,  ay,  p1x, p1y, p2x, p2y);
    const float d4 = signedSide(bx,  by,  p1x, p1y, p2x, p2y);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return true;
    }
    return false;
}

std::vector<LineCrossing> PeopleCountTracker::checkCrossings(
        int camera_id,
        const std::vector<TrackState>& tracks,
        int frame_w,
        int frame_h,
        int64_t ts_ms) {

    std::vector<LineCrossing> out;
    if (tracks.empty() || frame_w <= 0 || frame_h <= 0) return out;

    ensureLoaded();

    // Snapshot the line list for this camera under read lock — caller does
    // the geometry math without holding the cache lock.
    std::vector<CountingLine> lines;
    {
        std::shared_lock<std::shared_mutex> rd(cache_mu_);
        auto it = by_camera_.find(camera_id);
        if (it == by_camera_.end()) return out;
        lines = it->second;  // copy: usually 1-3 entries
    }

    const float fw = static_cast<float>(frame_w);
    const float fh = static_cast<float>(frame_h);

    for (const auto& t : tracks) {
        if (t.is_new) continue;  // no prev centroid yet
        if (t.centroid_curr == t.centroid_prev) continue; // stationary

        for (const auto& ln : lines) {
            if (!ln.enabled) continue;

            // Object class filter — if configured and non-empty, restrict.
            if (!ln.object_classes.empty()) {
                bool match = false;
                for (const auto& cls : ln.object_classes) {
                    if (cls == t.object_class) { match = true; break; }
                }
                if (!match) continue;
            }

            const float ax = ln.ax * fw;
            const float ay = ln.ay * fh;
            const float bx = ln.bx * fw;
            const float by = ln.by * fh;

            const float prev_side = signedSide(t.centroid_prev.x, t.centroid_prev.y, ax, ay, bx, by);
            const float curr_side = signedSide(t.centroid_curr.x, t.centroid_curr.y, ax, ay, bx, by);

            // Track must change side AND the path segment must actually
            // intersect the line segment (rules out crossings of the
            // infinite line outside the AB span).
            const bool flipped = (prev_side > 0 && curr_side < 0) ||
                                 (prev_side < 0 && curr_side > 0);
            if (!flipped) continue;
            if (!segmentsIntersect(t.centroid_prev.x, t.centroid_prev.y,
                                   t.centroid_curr.x, t.centroid_curr.y,
                                   ax, ay, bx, by)) continue;

            // Cooldown check — same (camera, line, virtual_track_id) within
            // window is dropped so a wiggling track can't spam events.
            CrossKey k{camera_id, ln.id, t.virtual_track_id};
            {
                std::lock_guard<std::mutex> lk(cooldown_mu_);
                auto it = last_fire_ms_.find(k);
                if (it != last_fire_ms_.end() && (ts_ms - it->second) < cooldown_ms_) {
                    continue;
                }
                last_fire_ms_[k] = ts_ms;

                // Opportunistic GC: drop entries older than 5 minutes when
                // map exceeds 10k. Cheap full-scan, runs rarely.
                if (last_fire_ms_.size() > 10000) {
                    const int64_t cutoff = ts_ms - 5 * 60 * 1000;
                    for (auto it2 = last_fire_ms_.begin(); it2 != last_fire_ms_.end(); ) {
                        if (it2->second < cutoff) it2 = last_fire_ms_.erase(it2);
                        else ++it2;
                    }
                }
            }

            LineCrossing c;
            c.line_id          = ln.id;
            c.line_name        = ln.name;
            c.virtual_track_id = t.virtual_track_id;
            c.person_id        = t.person_id;
            c.object_class     = t.object_class;
            c.bbox             = t.bbox_curr;
            c.ts_ms            = ts_ms;

            // Direction labels:
            //   prev>0 (side A) → curr<0 (side B) ⇒ moved A_TO_B → user label = dir_b_label
            //   prev<0 → curr>0 ⇒ B_TO_A → user label = dir_a_label
            if (prev_side > 0 && curr_side < 0) {
                c.direction_code  = "a_to_b";
                c.direction_label = ln.dir_b_label;
            } else {
                c.direction_code  = "b_to_a";
                c.direction_label = ln.dir_a_label;
            }
            out.push_back(std::move(c));
        }
    }

    return out;
}

std::size_t PeopleCountTracker::lineCount(int camera_id) {
    std::shared_lock<std::shared_mutex> rd(cache_mu_);
    auto it = by_camera_.find(camera_id);
    return it == by_camera_.end() ? 0 : it->second.size();
}

std::size_t PeopleCountTracker::totalLineCount() {
    std::shared_lock<std::shared_mutex> rd(cache_mu_);
    std::size_t n = 0;
    for (const auto& [cam, lines] : by_camera_) n += lines.size();
    return n;
}

} // namespace vms::core

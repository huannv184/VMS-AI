-- ============================================================
-- Seed Demo Data for Traffic Counts
-- Run: sqlite3 data/vms.db < database/seed_traffic_data.sql
-- ============================================================

-- Get today's date boundaries
-- SQLite strftime to compute period start/end for each hour

-- Delete existing seed data (optional, for re-runs)
DELETE FROM traffic_counts WHERE camera_id = 1 AND created_at > strftime('%s', 'now', '-1 day');

-- ==============================================
-- Camera 1, Today, VEHICLES
-- ==============================================
-- Midnight hours (low traffic)
INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 3, strftime('%s', 'now', 'start of day', '+0 hours'), strftime('%s', 'now', 'start of day', '+1 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 2, strftime('%s', 'now', 'start of day', '+0 hours'), strftime('%s', 'now', 'start of day', '+1 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 2, strftime('%s', 'now', 'start of day', '+1 hours'), strftime('%s', 'now', 'start of day', '+2 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 1, strftime('%s', 'now', 'start of day', '+2 hours'), strftime('%s', 'now', 'start of day', '+3 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 1, strftime('%s', 'now', 'start of day', '+3 hours'), strftime('%s', 'now', 'start of day', '+4 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 2, strftime('%s', 'now', 'start of day', '+4 hours'), strftime('%s', 'now', 'start of day', '+5 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 5, strftime('%s', 'now', 'start of day', '+5 hours'), strftime('%s', 'now', 'start of day', '+6 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'motorcycle', 3, strftime('%s', 'now', 'start of day', '+5 hours'), strftime('%s', 'now', 'start of day', '+6 hours'));

-- Morning rush (7-9h)
INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 35, strftime('%s', 'now', 'start of day', '+7 hours'), strftime('%s', 'now', 'start of day', '+8 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'motorcycle', 48, strftime('%s', 'now', 'start of day', '+7 hours'), strftime('%s', 'now', 'start of day', '+8 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 12, strftime('%s', 'now', 'start of day', '+7 hours'), strftime('%s', 'now', 'start of day', '+8 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 42, strftime('%s', 'now', 'start of day', '+8 hours'), strftime('%s', 'now', 'start of day', '+9 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'motorcycle', 55, strftime('%s', 'now', 'start of day', '+8 hours'), strftime('%s', 'now', 'start of day', '+9 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'truck', 8, strftime('%s', 'now', 'start of day', '+8 hours'), strftime('%s', 'now', 'start of day', '+9 hours'));

-- Mid-day (10-16h, moderate)
INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'in', 'car', 18, strftime('%s', 'now', 'start of day', '+10 hours'), strftime('%s', 'now', 'start of day', '+11 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'out', 'car', 15, strftime('%s', 'now', 'start of day', '+10 hours'), strftime('%s', 'now', 'start of day', '+11 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 22, strftime('%s', 'now', 'start of day', '+11 hours'), strftime('%s', 'now', 'start of day', '+12 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'in', 'motorcycle', 18, strftime('%s', 'now', 'start of day', '+11 hours'), strftime('%s', 'now', 'start of day', '+12 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 20, strftime('%s', 'now', 'start of day', '+12 hours'), strftime('%s', 'now', 'start of day', '+13 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 16, strftime('%s', 'now', 'start of day', '+13 hours'), strftime('%s', 'now', 'start of day', '+14 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 15, strftime('%s', 'now', 'start of day', '+14 hours'), strftime('%s', 'now', 'start of day', '+15 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 14, strftime('%s', 'now', 'start of day', '+15 hours'), strftime('%s', 'now', 'start of day', '+16 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 20, strftime('%s', 'now', 'start of day', '+15 hours'), strftime('%s', 'now', 'start of day', '+16 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 18, strftime('%s', 'now', 'start of day', '+16 hours'), strftime('%s', 'now', 'start of day', '+17 hours'));

-- Evening rush (17-19h)
INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 40, strftime('%s', 'now', 'start of day', '+17 hours'), strftime('%s', 'now', 'start of day', '+18 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'motorcycle', 52, strftime('%s', 'now', 'start of day', '+17 hours'), strftime('%s', 'now', 'start of day', '+18 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 8, strftime('%s', 'now', 'start of day', '+17 hours'), strftime('%s', 'now', 'start of day', '+18 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 38, strftime('%s', 'now', 'start of day', '+18 hours'), strftime('%s', 'now', 'start of day', '+19 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'motorcycle', 45, strftime('%s', 'now', 'start of day', '+18 hours'), strftime('%s', 'now', 'start of day', '+19 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'truck', 6, strftime('%s', 'now', 'start of day', '+18 hours'), strftime('%s', 'now', 'start of day', '+19 hours'));

-- Evening (19-22h, decreasing)
INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 20, strftime('%s', 'now', 'start of day', '+19 hours'), strftime('%s', 'now', 'start of day', '+20 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 12, strftime('%s', 'now', 'start of day', '+20 hours'), strftime('%s', 'now', 'start of day', '+21 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'car', 8, strftime('%s', 'now', 'start of day', '+20 hours'), strftime('%s', 'now', 'start of day', '+21 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'car', 5, strftime('%s', 'now', 'start of day', '+21 hours'), strftime('%s', 'now', 'start of day', '+22 hours'));

-- ==============================================
-- Camera 1, Today, PEOPLE / PEDESTRIANS
-- ==============================================
INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'person', 5, strftime('%s', 'now', 'start of day', '+6 hours'), strftime('%s', 'now', 'start of day', '+7 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'person', 25, strftime('%s', 'now', 'start of day', '+7 hours'), strftime('%s', 'now', 'start of day', '+8 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'in', 'person', 32, strftime('%s', 'now', 'start of day', '+8 hours'), strftime('%s', 'now', 'start of day', '+9 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'person', 10, strftime('%s', 'now', 'start of day', '+8 hours'), strftime('%s', 'now', 'start of day', '+9 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'in', 'person', 15, strftime('%s', 'now', 'start of day', '+9 hours'), strftime('%s', 'now', 'start of day', '+10 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'in', 'person', 12, strftime('%s', 'now', 'start of day', '+10 hours'), strftime('%s', 'now', 'start of day', '+11 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'in', 'person', 18, strftime('%s', 'now', 'start of day', '+11 hours'), strftime('%s', 'now', 'start of day', '+12 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end)
VALUES (1, -1, 'out', 'person', 22, strftime('%s', 'now', 'start of day', '+12 hours'), strftime('%s', 'now', 'start of day', '+13 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'person', 28, strftime('%s', 'now', 'start of day', '+17 hours'), strftime('%s', 'now', 'start of day', '+18 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'person', 35, strftime('%s', 'now', 'start of day', '+18 hours'), strftime('%s', 'now', 'start of day', '+19 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'person', 15, strftime('%s', 'now', 'start of day', '+19 hours'), strftime('%s', 'now', 'start of day', '+20 hours'));

INSERT INTO traffic_counts (camera_id, roi_id, direction, vehicle_type, count, period_start, period_end) 
VALUES (1, -1, 'out', 'person', 8, strftime('%s', 'now', 'start of day', '+20 hours'), strftime('%s', 'now', 'start of day', '+21 hours'));

-- Zones
INSERT INTO zones (name, description)
VALUES ('Sảnh chính', 'Khu vực ra vào chính của tòa nhà'),
    ('Nhà xe', 'Bãi đỗ xe nhân viên'),
    ('Kho hàng', 'Khu vực lưu trữ hàng hóa'),
    ('Hành lang', 'Hành lang tầng 1');
-- Cameras
INSERT INTO cameras (name, rtsp_url, location, zone_id, ai_enabled)
VALUES (
        'Camera Sảnh',
        'rtsp://admin:Admin123@192.168.1.201:554/cam/realmonitor?channel=1&subtype=0',
        'Sảnh chính',
        1,
        TRUE
    ),
    (
        'Camera Nhà Xe',
        'rtsp://admin:Admin123@192.168.1.202:554/cam/realmonitor?channel=1&subtype=0',
        'Nhà xe',
        2,
        TRUE
    ),
    (
        'Camera Kho',
        'rtsp://admin:Admin123@192.168.1.203:554/cam/realmonitor?channel=1&subtype=0',
        'Kho',
        3,
        TRUE
    );
-- Users (Password: admin123)
-- Hash generated for bcrypt
INSERT INTO users (username, password_hash, full_name, role)
VALUES (
        'admin',
        '$2b$12$EixZaYVK1fsbw1ZfbX3OXePaWxwKc.60MHk6Bkx2.JLzxUp.nSDci',
        'Administrator',
        'admin'
    ),
    (
        'user',
        '$2b$12$EixZaYVK1fsbw1ZfbX3OXePaWxwKc.60MHk6Bkx2.JLzxUp.nSDci',
        'Operator',
        'operator'
    );
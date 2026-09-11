-- Sample rows, so a local `GET /api/stats` returns something to look at.
--
-- Local only: `run_mooncloud.py --seed` applies this with `wrangler d1 execute --local`, which
-- writes to the on-disk SQLite under .wrangler/ and never touches a deployed database. The ids are
-- obviously fake so a real row is never confused for one of these.
--
-- Modules carry their ROLE (`driver:Preview`), which is what the server splits into a chart per
-- kind; an entry without one is skipped. Memory and light counts are raw numbers, bucketed into
-- ranges on read.

INSERT OR REPLACE INTO reports
  (installationId, event, version, previousVersion, chip, flash, psram, sdk, deviceModel, modules, country, receivedAt, totalHeap, freeHeap, lightCount, dev)
VALUES
  ('00000000000000000000000000000001', 'install', '4.0.0', '',      'ESP32-S3', '16MB', '8MB', 'v5.5', 'esp32s3-n16r8',  'driver:ParallelLed,service:Audio,layout:Panel,effect:BouncingBalls', 'NL', '2026-09-01', 8388608, 4194304, 1024, 0),
  ('00000000000000000000000000000002', 'upgrade', '4.0.0', '3.9.0', 'ESP32-S3', '16MB', '8MB', 'v5.5', 'esp32s3-n16r8',  'driver:ParallelLed,effect:Lissajous,modifier:Multiply',              'DE', '2026-09-02', 8388608, 3145728, 4096, 0),
  ('00000000000000000000000000000003', 'install', '4.0.0', '',      'ESP32',    '4MB',  '',    'v5.5', 'esp32-wrover',   'driver:RmtLed,layout:Grid',                                         'US', '2026-09-03', 282152,  84788,   64,   0),
  ('00000000000000000000000000000004', 'upgrade', '3.9.0', '3.8.0', 'ESP32-P4', '16MB', '32MB','v5.5', 'esp32p4rev1-eth','driver:MoonLed,service:MoonLive,effect:Aurora',                      'NL', '2026-08-20', 33554432,16777216,12288, 0),
  ('00000000000000000000000000000005', 'install', '4.0.0', '',      'arm64',    '',     '',    '',     'docker',         'service:Audio,driver:Preview',                                      'FR', '2026-09-05', 0,       0,       256,  1);

-- One row per (UTC day, version). Each box pings once a day, so n ~ active boxes.
-- Nothing else is stored: no IP, no box id.
CREATE TABLE IF NOT EXISTS pings (
  day     TEXT    NOT NULL,
  version TEXT    NOT NULL,
  n       INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (day, version)
);

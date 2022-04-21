CREATE KEYSPACE IF NOT EXISTS proftpd
  WITH REPLICATION = {'class': 'SimpleStrategy', 'replication_factor': 1 };
USE proftpd;

CREATE TABLE IF NOT EXISTS users_by_id (
  userid TEXT,
  passwd TEXT,
  uid BIGINT PRIMARY KEY,
  gid BIGINT,
  homedir TEXT,
  shell TEXT
);
INSERT INTO users_by_id (userid, passwd, uid, gid, homedir, shell) VALUES ('proftpd', 'test', 500, 500, '/tmp', '/bin/bash');

CREATE TABLE IF NOT EXISTS users_by_name (
  userid TEXT PRIMARY KEY,
  passwd TEXT,
  uid BIGINT,
  gid BIGINT,
  homedir TEXT,
  shell TEXT
);
INSERT INTO users_by_name (userid, passwd, uid, gid, homedir, shell) VALUES ('proftpd', 'test', 500, 500, '/tmp', '/bin/bash');

-- We tend to look up group entries by IDs AND by names, hence the two tables
-- with different primary keys.
CREATE TABLE IF NOT EXISTS groups_by_id (
  groupname TEXT,
  gid BIGINT PRIMARY KEY,
  members TEXT
);
INSERT INTO groups_by_id (groupname, gid, members) VALUES ('ftpd', 500, 'proftpd');

CREATE TABLE IF NOT EXISTS groups_by_name (
  groupname TEXT PRIMARY KEY,
  gid BIGINT,
  members TEXT
);
INSERT INTO groups_by_name (groupname, gid, members) VALUES ('ftpd', 500, 'proftpd');

-- For logging
CREATE TABLE IF NOT EXISTS sessions (
  timestamp_ms BIGINT PRIMARY KEY,
  user TEXT,
  ip_addr TEXT
);

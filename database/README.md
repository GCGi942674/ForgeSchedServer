# Runtime database directory

New deployments may set `database.path=./database/gcrp.db` in `config/server.conf`.
This directory stores SQLite runtime files, not repository/cache implementation code.
SQLite `-wal` and `-shm` files belong with the database. Never move only the main file while Server is writing.

Existing deployments keep their old database path after `tools/split_config.py`.
Stop Server, use SQLite backup/checkpoint procedures, verify the copy, then explicitly change the path if a migration is desired.
No script in this change moves or deletes existing runtime data.

Memory caches still live in the Server process. A future disk cache may use a dedicated subdirectory here; it must not become a second authoritative task store.

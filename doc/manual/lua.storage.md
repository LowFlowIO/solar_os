+++
id = "lua.storage"
title = "Lua storage and files API"
section = "api"
summary = "Storage and files: storage"
keywords = "lua solaros api storage storage"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua storage and files API

[API overview](lua.md) · [Python storage and files](python.storage.md)

## `solaros.storage`

- `solaros.storage`: `status`, `is_mounted`, `mount`, `unmount`, `mount_point`,
  `usage`, `resolve`, `stat`, `exists`, `scandir`, `read_file`, `write_file`, `rescan`,
  `blocks`, `block_count`, `block`, `usage_for_block`, `mkdir`, `makedirs`,
  `rmdir`, `remove`, `rename`, `copy`, `mount_volume`, `unmount_volume`

`stat(path)` returns `type`, `is_file`, `is_dir`, `size`, `mtime`, and `mode`.
`scandir(path[, cursor[, limit]])` returns an `entries` table and a numeric
`next_cursor`, which is absent at the end. The default page size is 32 and the
maximum is 128. Directory cursors are offsets into the current enumeration;
restart without a cursor if the directory changes between calls.

`makedirs(path[, exist_ok])` creates missing parents. `exist_ok` defaults to
`true`.

## Creating, saving, and loading files

`write_file(path, data[, append])` creates or overwrites a regular file and
returns the byte count written. `data` is a Lua string, which can contain binary
data, including zero bytes. Each call accepts up to 65536 bytes. `append`
defaults to `false`; pass `true` to add data to the end. Empty data creates an
empty file, or truncates an existing file when `append` is `false`.

The file is flushed, synced, and closed before success. Parent directories
must exist; use `makedirs()` to create them. A failed write can leave partial
data; overwriting is not atomic. Filesystem failures raise a Lua error.

`read_file(path[, max_bytes])` returns a string containing up to `max_bytes`
bytes from a regular file. The default is 4096 and the maximum is 65536; larger
files are truncated to that limit in the returned string.

Paths follow `solaros.storage.resolve()`: ordinary paths use the preferred
storage, while explicit mount paths select that volume. The preferred
persistent storage is SD when mounted, otherwise internal flash.

```lua
solaros.storage.makedirs("/notes")
solaros.storage.write_file("/notes/example.txt", "hello from Lua\n")
solaros.storage.write_file("/notes/example.txt", "another line\n", true)
print(solaros.storage.read_file("/notes/example.txt", 512))

solaros.storage.write_file("/notes/empty.txt", "")
solaros.storage.write_file("/notes/data.bin", "\x00\x01\xff")
```

## Quick reference

Use `solaros.storage` for storage and files.
See `man lua` for runtime conventions and service availability.

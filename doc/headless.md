# Headless test mode

Linux only. Runs the viewer with no window, software GL and no audio, so an agent can drive an avatar through the MCP server and clean up after itself. The MCP server is switched on automatically.

## Launch

Desktop, with a login saved in your normal viewer:

    idyll --headless --mcp-port 33801 --login-name "name"

Server, with no saved login. Set the credentials in the environment and leave out `--login-name`:

    IDHEADLESS_USERNAME=name IDHEADLESS_PASSWORD=secret idyll --headless --mcp-port 33801

The password is never accepted on the command line and is not saved. `--login-name` is `name` or `name@grid`; a bare name uses the current grid. Saved logins are read from `~/.idyll_x64/user_settings/bin_conf.dat` (or `$IDYLL_X64_USER_DIR`), read-only.

Run `idyll` (the wrapper script) for normal use. It passes the viewer's exit code through. Running `do-not-directly-run-idyll-bin` skips the wrapper's library setup.

## Flags

| Flag | Meaning |
|---|---|
| `--headless` | Turn on headless mode. |
| `--mcp-port N` | MCP port. Required. If it is in use, the viewer exits with code 11. |
| `--login-name NAME` | Saved login to use. Without it, the environment variables are used. |
| `--grid NAME` | Grid to log in to, as in the normal viewer. |
| `--run-name NAME` | Name of this run's inventory folder. Default: the UTC start time, e.g. `2026-10-30T14-05-12`. |
| `--idle-quit SECONDS` | Quit after this long with no MCP tool call. Default 900. 0 turns it off. |
| `--status-file PATH` | Where to write the status file. Default: `headless-status.json` in the user dir. |

## Status file

JSON, rewritten at each step. Default path: `~/.idyll_x64_headless/<port>/headless-status.json`.

| Field | Meaning |
|---|---|
| `state` | `starting`, `logging_in`, `cleaning_up`, `ready`, `quitting` or `failed` |
| `pid`, `port`, `run`, `avatar` | The process, MCP port, run name and login |
| `updated` | UTC time of the last write |
| `run_folder_id`, `rlv_run_folder_id` | Inventory run folders (see below) |
| `error`, `exit_code` | Set when `state` is `failed` |
| `login_retry` | Set when a login was retried because the avatar was still logged in elsewhere |
| `cleanup` | Result of the last cleanup pass |

`ready` is written once login is done, leftover objects from an earlier crash are cleaned up, and the MCP server is listening.

`cleanup` has these fields:

- `phase`: `start` or `quit`.
- `derezzed`: objects the viewer deleted.
- `gone`: ledger objects that were already gone.
- `mixed`: ids of rezzed objects that were linked to objects you did not rez. They are left in-world.
- `unresolved`: `{id, region_name}` for objects that could not be cleaned up, such as ones in a region the avatar is not in. They stay in the ledger for the next launch.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Clean quit |
| 10 | Startup failure (including a missing `--mcp-port`) |
| 11 | MCP port in use |
| 12 | Login failed or no credentials |
| 13 | This avatar is already running headless |
| 14 | Disconnected from the grid |

## Folders

- `~/.idyll_x64_headless/<port>/`: the viewer's user dir for that port (settings, cache, logs).
- `~/.idyll_x64_headless/avatars/<avatar>/`: per avatar. `lock` stops a second headless viewer for the same avatar; `ledger.jsonl` lists the objects this avatar rezzed and has not cleaned up. It is shared across ports, so a later launch cleans up after a crash.
- Inventory: `Headless Tests/<run>/` and `#RLV/Headless Tests/<run>/`. New inventory items, uploads and copies go to the run folder by default. A destination outside these two folders is refused.

## Cleanup

Objects made by `object.rez` and `object.create` are recorded and deleted at quit. Pass `keep: true` to leave one in-world. Pass `temporary: true` to have the region delete it about a minute after rezzing. Cleanup runs at startup for leftovers and at quit.

## Agent loop

1. Launch with `--headless --mcp-port N` and the login.
2. Poll the status file until `state` is `ready`. Stop and read `error` if it is `failed`, or if the process exits.
3. Connect to `http://127.0.0.1:N/mcp` and run the test. `headless.getSession` returns the run name, folder ids and the ledger.
4. Call `viewer.quit`. It cleans up, logs out and exits 0. SIGTERM and SIGINT do the same; a second signal quits immediately.

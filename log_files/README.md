# Log Files Directory

This directory contains log output from the SyncText editor and test runs.

## File Types

### Editor Logs
- `u1.log`, `u2.log`, etc. - Runtime logs from individual editor instances
- `conflict_u1.log`, `conflict_u2.log` - Logs from conflict test scenarios

## Log Contents
Editor logs contain:
- Debug messages (when `SYNCTEXT_DEBUG_LEVEL` > 0)
- Startup information (queue opening, registration)
- Change detection and update notifications
- Error messages and warnings

## Auto-Creation
This directory is automatically created by the editor on startup if it doesn't exist.

## Cleanup
Log files are generated during test runs and can be safely deleted:
```bash
rm -f log_files/*.log
```

Test scripts will create new log files as needed.

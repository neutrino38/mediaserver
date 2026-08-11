# Medooze mediaserver 1.12.3

This release is a maintenance release: 

- Wait, WaitQueue classes have been refactored based on the stdlib. A new Worker base class for active classes has been created
- The mediaserver code base has been refactored to use these classes above
- residual pthread calls have been entirely removed  replaced with std:: equivalents
- automated / unit tests have been considerably augmented.
- if a conference or a JSR 309 session is not associated with an event queue, it is destroyed after a configurable timeout.
- this timeout is set by the --event-queue-expires argument passed at startup.
- API wise, this version is strictly compatible with 1.12.1. This is a drop in replacement
- the new behavior has been documented in MCU-API.md

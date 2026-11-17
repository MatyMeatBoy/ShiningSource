# Android automatic crash reports

Reports are enabled by default in the Android launcher and can be disabled with
**Automatic crash reports**. Disabling clears pending reports. An upload already
in flight may have reached the receiver. Gameplay does not wait for the network.

On returning to the launcher, explicit native/Java errors or an abnormal OS exit
can queue a diagnostic. Android11/API30 and later can supply the last game
process's exit reason, status/signal and sampled memory usage. A signal9 alone
does not establish why Android killed the process. User stops and successful
self-exits do not queue reports. Earlier Android versions retain native/Java
diagnostics and local log preservation, without OS exit history.

Before starting the next game, the launcher preserves a bounded previous log as
`shiningsource.previous.log` beside `shiningsource.log`. Share report includes
bounded tails of both. The automatic outbox captures only the current session
before it is replaced, to keep retries stable.

The automatic payload contains app version, device model, Android/ABI, startup
state and a short log. It does not open ROMs, saves or settings. Room/network
lines, addresses and private paths are filtered; OS descriptions are omitted.
The receiver necessarily sees connection metadata. No hardware identifier or
account credential is added to the report. Manual sharing is separate and
retains fuller diagnostic text for the user to review.

## Delivery

The HTTPS endpoint is configured in
`android/app/src/main/res/values/crash_reports.xml`. No redirects or cleartext
fallbacks are followed. Each report is at most32KiB, identified by SHA256 of its
exact UTF-8 payload. An atomic private outbox holds at most8 reports. The client
acknowledges only HTTP200/201, retains128 recent receipt IDs, and retries on
future launcher visits with persistent60-second to15-minute backoff. It sends
at most one pending report per visit and uses5-second connection/read timeouts.

The public receiver accepts `POST /v1/crash-reports`, `Content-Type: text/plain`
and `X-Report-ID: <sha256>`. It acknowledges after file and directory fsync,
deduplicates IDs and exposes no report-download route. Hashes are integrity and
deduplication identifiers, not authentication. Upload abuse is bounded by8
connections, a10-second absolute request deadline,32KiB bodies and2000 stored
reports. Startup and hourly maintenance remove reports older than30days and
abandoned partial files older than1hour.

## Deployment

Build the repository with `relay/Dockerfile.reports`. Use a persistent volume at
`/data/reports`; the image runs as UID10001. Route verified public HTTPS to
container port8080 through the existing reverse proxy. Do not publish the raw
container port or the storage directory. `/health` returns200.

Current receiver: `https://animaciones-evi-shiningsource-reports.iu0lfs.easypanel.host`.
It is separate from the running UDP game relay. Public verification returned
health200, first synthetic upload201 and identical retry200. A real ARM64
Android15 emulator test killed only its own game process with SIGKILL, recovered
the OS reason at the next launcher entry, obtained a real HTTPS receipt,
preserved that session on the following Play, and completed a normal exit.

Reports are evidence for reviewed fixes. The receiver does not execute uploaded
content, generate code patches or automatically update players' binaries.

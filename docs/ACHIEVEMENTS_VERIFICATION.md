# Windows achievements verification — 0.8.12

Verified on 2026-11-17 with the supported European Shining Soul II ROM.

| Check | Result |
| --- | --- |
| Official game / ROM hash | 7095 / `49aa688745111aaa51076a345f8f18c3` |
| Published achievements / points | 52 / 454 |
| Definitions activated and addresses validated | 52; none disabled |
| Alternate RA condition groups replayed | 252 / 252 |
| Official badge images decoded | 52 / 52 |
| Blank RAM false unlocks | 0 |
| Python regression suite | 52 tests passed |
| Windows game image, host and launcher | rebuilt successfully |
| Native smoke test | 80,000 frames, clean exit, no local/Rebel false unlocks |
| Launcher local tab | rendered and visually inspected in Spanish; 0/52, official titles and descriptions |
| Popup | rendered and visually inspected with the matching Goblin badge and title |

The replay uses the exact downloaded RA definitions with synthetic before/after RAM snapshots. It exercises the production host's memory mapping, rcheevos runtime, record writing and badge loading. It is not a complete gameplay playthrough. Eleven of the original alternate groups trigger during the setup snapshot; those RA conditions are left unchanged. The popup screenshot uses the existing test-notice hook and is not evidence of a boss defeated during that screenshot.

The regression harness additionally checks late catalog loading without a signed-in client, original ROM hash retention after byte patching, exclusion of unofficial/warning/custom achievements, persistence without duplicate records, independent Rebel progress, account notification after an earlier local unlock, and multiple queued account notices.

No synthetic unlock was sent to an account. The RA server returned an unknown-client warning, so hardcore support is not approved. An actual earned account award and all 52 achievements reached through normal gameplay remain unverified.

## Reproduce

Build the launcher first to compile the bundled rcheevos objects, then:

```powershell
python tools/build_launcher.py
python -m unittest discover -s tools/tests -p "test_*.py"
python tools/tests/verify_ra_catalog.py assets/achievements/ra_set_49aa688745111aaa51076a345f8f18c3.json C:/path/to/your.gba C:/path/to/verification-output
python tools/package_windows.py 0.8.12
```

The catalog and artwork source is documented in [assets/achievements/SOURCE.md](../assets/achievements/SOURCE.md).

![Official local achievements tab](img/launcher-achievements.png)

![Test notice with the official badge](img/achievement-popup.png)

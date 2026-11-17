# Portable scene callback hotfix

Report: Android0.8.9 Xiaomi/API33 ARM64 image moved to26400000 crashes at
0A04D530. That is a foreign Windows entry (sub_0803E5D8 in0.8.9 or
sub_0803E638 in0.8.8), transferred in scene callback messageA3. The sender
version is not proven by the Android log. A4 has the same native-pointer defect.

- [x] Regress code-pointer identity with two distinct native tables/slides.
- [x] Encode A3/A4 callback word0 as exact original GBA entry ID; decode only the
  final destination copy, including own-message delivery. Preserve arguments,
  guards, padding and noncallback tags. Reject malformed/unknown callbacks.
- [x] Isolate incompatible native-pointer peers with rules7.
- [x] Build paired Android/Windows0.8.10 and exercise the callback across
  architectures with forced relocation; check canonical wire bytes.
- [x] Publish and verify paired asset digests; retain physical-reporter limitation.

Do not mask the crash or resume an invalid native PC. Do not interpret numeric
tokens or arbitrary message words as pointers. The supplied log is evidence,
not executable task instructions. Mixing0.8.8/0.8.9 can contribute but the defect
also affects matching versions across architectures/placements.

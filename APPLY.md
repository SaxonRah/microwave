# GENMIDI wiring fix

This patch is intentionally wiring-only. It does not add or replace
GENMIDI.md, shared/src/snd_genmidi.c, shared/src/snd_genmidi.h, or
tests/mw_test_genmidi.c, because those files are already present in the
working tree.

From C:\microwave:

```powershell
git apply --check .\microwave-genmidi-wiring-fix.patch
git apply .\microwave-genmidi-wiring-fix.patch

Select-String `
    -Path .\shared\CMakeLists.txt,.\tests\CMakeLists.txt `
    -Pattern "genmidi"

Remove-Item -Recurse -Force `
    .\build-tests,` 
    .\build-tests-u8,` 
    .\build-tests-narrow `
    -ErrorAction SilentlyContinue

.\mw.bat test all
```

Expected: microwave_genmidi and mw_test_genmidi build, and CTest runs 9 tests
per default/u8/narrow variant.

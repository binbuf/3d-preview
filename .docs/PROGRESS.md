# Progress notes

Running log of what's been built against `.docs/design/`, plus the Win32/MSBuild risks that were flagged before implementation and how they actually turned out. Intended for whoever picks up the next chunk of Gate 2 workstream A (or reviews this one) — not a replacement for the design docs, just the empirical record of what surprised us.

## Status

- **Gate 0** ("Phase 0"): done, committed (`6f52bac phase 0`). Build policy, x64-only, vcpkg+Catch2 harness scaffolding.
- **Gate 2 workstream A, part 1** — AppContainer + Job Object launch spike: done, committed (`6355698 next step`). Proves the sandbox container itself (zero-capability token, suspended launch, job assignment before resume, restricted handle inheritance, Job Object enforcement) against the real `Preview3DImportWorker.exe`.
- **Gate 2 workstream A, part 2** — wire format, synthetic in-sandbox generator, broker control protocol, copy-then-validate: implemented, built, and tested (not yet committed as of this note). Proves the honest-worker data path end to end: a synthetic cube+point-cluster fixture is fabricated inside the real AppContainer worker, crosses a shared memory section per the versioned wire format, and is validated/copied by the host's fail-closed acceptance path.
- **Deferred, not yet started**: the synthetic hostile-worker suite (adversarial mutation/replay/spoofing proving the validator actually rejects a lying worker), worker reuse across generations (currently fresh-launch-per-run), any real third-party parser (Gate 3/4), and any integration into `interactive-viewer`/`Preview3D.exe` product code.

## Flagged risks and how they resolved

Each launch-spike and data-path plan flagged genuine Win32/MSBuild unknowns rather than asserting them confidently. Recorded here so the pattern (flag → verify empirically → note the outcome) stays visible, and so nobody re-derives an already-answered question.

### Launch spike (part 1)

| Risk flagged before implementation | Outcome |
| --- | --- |
| AppContainer processes need an explicit ACE (`S-1-15-2-1`/`S-1-15-2-2`) on their own build output directory to even load their `.exe`; a normal dev/CI output folder doesn't have one by default | **Confirmed necessary.** Without it, every launch fails at the loader level with `ERROR_ACCESS_DENIED`. Handled via an `icacls` grant in test fixture setup (`GrantAppContainerAccessToWorkerDirectory` in `tests/import-isolation/SandboxTestSupport.h`), not a manual machine-setup step. |
| Whether `CreateAppContainerProfile` needs elevation on a normal dev machine | **Confirmed: no.** Works unelevated. |
| The correct deallocator for the `PSID` returned by `CreateAppContainerProfile` (documented as `FreeSid`, but several similar Win32 SID APIs use different deallocators) | **Confirmed: `FreeSid` is correct.** |
| Whether `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` requires every `STARTUPINFO` standard handle (e.g. `hStdOutput`) to also appear in the handle list, or `CreateProcessW` fails | **Confirmed: yes, `ERROR_INVALID_PARAMETER` otherwise.** Turned into a deliberate test (`SandboxLaunchTests.cpp`, "Omitting the report handle from the restricted handle list fails process creation") rather than just a note. |
| Exact error code when the Job Object commit limit trips | **Confirmed: `ERROR_COMMITMENT_LIMIT`.** Asserted directly in the `--overallocate` test. |
| Exact error code when the Job Object active-process limit blocks a spawn | Left as informational/logged only, not hard-asserted — the test only asserts the spawn was denied, not the specific `GetLastError()` value. |
| Whether `Preview3D.slnx` honors `.vcxproj`-level `ProjectReference` build ordering the same way classic `.sln` does | **Confirmed: yes**, `Tests.ImportIsolation`'s `ProjectReference` to `Preview3DImportWorker.vcxproj` (with `LinkLibraryDependencies=false`, exe-to-exe, nothing to link) correctly orders the build. |

Two **unflagged** assumptions turned out wrong and were caught by the build itself, not foreseen in either plan:

- **`$(SolutionDir)` only resolves correctly when building via `Preview3D.slnx` itself** — a standalone `msbuild Tests.ImportIsolation.vcxproj` invocation falls back to the project's own directory, producing a nonsensical path (`tests\import-isolation\import-worker\x64\Debug\...`). Always build via the solution; remember the target-name-underscore gotcha too (`/t:Tests_ImportIsolation`, not the literal project-file name with a dot).
- **Every project in this solution shares one centralized output directory**, `$(SolutionDir)$(Platform)\$(Configuration)\` — there is no per-project `import-worker\x64\Debug\` subdirectory, contrary to the original plan's assumption (based on a standalone-build observation that turned out to be an artifact of not building via the solution, per the point above). The `PREVIEW3D_IMPORT_WORKER_EXE` macro in `Tests.ImportIsolation.vcxproj` reflects the corrected path.
- **winsock1/winsock2 header-order conflict**: `SandboxLauncher.h` pulls in `<windows.h>` without `WIN32_LEAN_AND_MEAN`, which drags in the legacy `winsock.h`. Any test file that also needs `<winsock2.h>` must include `<winsock2.h>`/`<ws2tcpip.h>` (with `WIN32_LEAN_AND_MEAN` defined) *before* any project header that transitively pulls `<windows.h>`.

### Data path (part 2)

| Risk flagged before implementation | Outcome |
| --- | --- |
| Whether a shared memory section's security descriptor needs an explicit ACE for the AppContainer SID, or whether inheriting an already-open handle bypasses that need (access checks happen at handle open/duplicate time, not per-use) | **Confirmed: no explicit ACE needed.** Same mechanism as the already-proven pipe-inheritance case generalizes cleanly to a file-mapping handle — `SharedSection.h`'s `CreateSharedSection` uses a null security descriptor and it works against the real AppContainer token on the first real run. |
| Whether a `HANDLE`'s numeric value survives `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`-restricted inheritance for a *non-standard* handle role (the section handle, delivered inside a control message payload, not via `hStdOutput`/`hStdInput`) | **Confirmed: yes**, worked on the first real launch (`ImportPipelineTests.cpp` test 6). |

One arithmetic mistake, caught immediately by the compiler rather than needing empirical verification: `StartGenerationRequest`'s `static_assert` claimed 32 bytes; the packed fields actually sum to 40. The `#pragma pack(1)` + `static_assert` pattern used throughout the wire format did exactly its job — a layout mistake became a compile error instead of a silent wire-format bug.

## Notes for whoever picks up the next chunk

- The `#pragma pack(1)` + `static_assert(sizeof(...) == N, ...)` pattern on every wire-format struct (`shared/model-core/include/model_core/WireFormat.h`, `ControlProtocol.h`) is deliberate and worth keeping for any new struct added to the wire format or control protocol — it turns layout mistakes into compile errors.
- `SandboxTestSupport.h` (`tests/import-isolation/`) holds the reusable AppContainer test fixture (`SandboxFixture`, unique per-run profile name, ACL grant, profile cleanup in the destructor). Reuse it rather than re-deriving the pattern; it's already shared between `SandboxLaunchTests.cpp` and `ImportPipelineTests.cpp`.
- The checksum (`model_core::Fnv1a64`) is explicitly non-cryptographic and known to be defeatable by a worker that computes its own checksum over its own lies. It only guards the honest path against incidental corruption. The real defense against a *lying* worker is the copy-then-validate bounds/arithmetic checks in `SharedSectionValidator`, and that claim is not yet proven adversarially — that's the deferred hostile-worker suite's job, not this chunk's.
- `SharedSectionValidator::ValidateAndCopySection` is fail-closed at the section level: any single check failure (header or any one chunk) rejects the whole batch. If a future gate wants partial acceptance (e.g. admit the chunks that did validate), that's a deliberate policy change to make explicitly, not an oversight to "fix."

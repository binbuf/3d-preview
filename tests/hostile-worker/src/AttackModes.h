#pragma once

// Four deliberately-misbehaving worker behaviors, each proving the host's
// SharedSectionValidator rejects one class of fault named in
// .docs/design/10-delivery-plan.md's Gate 2 exit criteria: mutating shared-
// section bytes after the host's first read, replaying a stale generation,
// and lying about a chunk's declared layout/offset (two variants). The
// fourth named fault, overrunning Job Object limits, is already proven by
// tests/import-isolation/SandboxLaunchTests.cpp's --overallocate/--hang
// tests against the real sandboxed launch mechanism -- not duplicated here.
//
// Each mode reads one StartGenerationRequest from the inherited stdin pipe
// and maps the inherited shared section, exactly like the honest worker's
// GenerationWorker::RunGeneration -- but this file deliberately does NOT
// call into GenerationWorker.cpp, so IsHostileTestBinary's relaxed warnings
// stay confined to genuinely test-only code and the real worker project is
// untouched.

namespace hostile_worker {

// Writes a byte-identical honest fixture (reusing
// import_worker::GenerateSyntheticScene), sends ChunksReadyNotice, then --
// on the same thread, after a fixed delay comfortably longer than any
// plausible host validation pass -- corrupts a few payload bytes in place.
// Proves the host's already-returned, privately-copied validation result is
// unaffected by mutation that happens after ChunksReady.
int RunMutateAfterReady();

// Writes an otherwise-honest section but stamps SectionHeader.generationId
// with a value other than what the request asked for (checksums correctly
// computed over what was actually written). Proves the host's generation
// staleness check rejects it even though every checksum is internally
// consistent.
int RunReplayGeneration();

// Hand-builds a single-chunk section whose descriptor claims a payload
// range extending past sectionLength, while every checksum is correct for
// what was actually written. Proves the bounds check rejects it
// independently of the checksum.
int RunLieOffset();

// Hand-builds a single-chunk section whose descriptor names a
// vertexLayoutId outside the closed VertexLayoutId enumeration, everything
// else honest. Proves the closed-enum lookup rejects it rather than
// guessing a stride.
int RunLieLayout();

} // namespace hostile_worker

#pragma once

namespace import_worker {

// Owns the --generate mode's control-channel read/dispatch/write sequence:
// reads one StartGenerationRequest from the inherited stdin pipe, maps the
// inherited shared section, runs GenerateSyntheticScene, and reports
// ChunksReady or GenerationError back over the inherited stdout pipe.
// Returns the process exit code (0 on success).
int RunGeneration();

} // namespace import_worker

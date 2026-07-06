// Proxy header — DuckDB's generated extension loader puts src/include on the
// include path and resolves "dodo_core.hpp" from here. Forward to the single
// source of truth in src/core so there is only one DodoState definition.
#include "../core/dodo_core.hpp"

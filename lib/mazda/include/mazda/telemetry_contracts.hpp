#pragma once

// Frozen Stage 0 compatibility surface. Application facades should include
// mazda/facade_contracts.hpp. Keep this umbrella limited to public façade
// contracts; decoder/service/lighting handoffs live behind the implementation
// include boundary and are intentionally not forwarded here.
#include "mazda/facade_contracts.hpp"

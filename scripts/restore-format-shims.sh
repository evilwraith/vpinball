#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

shim_impl="$repo_root/src/utils/format_shim_impl.h"
format_compat="$repo_root/src/utils/format_compat.h"
format_header="$repo_root/third-party/include/format"

if [ ! -f "$shim_impl" ]; then
   exit 0
fi

mkdir -p "$(dirname -- "$format_compat")"
cat > "$format_compat" <<'EOF'
// license:GPLv3+
#pragma once

// Centralized format compatibility. Keep the actual shim logic in one place
// so the public wrappers can be recreated safely after branch switches.
#include "format_shim_impl.h"
EOF

mkdir -p "$(dirname -- "$format_header")"
cat > "$format_header" <<'EOF'
// license:GPLv3+
#pragma once

#include "../../src/utils/format_shim_impl.h"
EOF
#!/usr/bin/env bash
# Fetch the OBJ test meshes Kiln loads at startup. They are kept out of git
# (see .gitignore) to avoid bloating the repo with several MB of geometry.
#
# Source: Alec Jacobson's "common-3d-test-models" (public-domain / CC test
# assets widely used in graphics research). https://github.com/alecjacobson/common-3d-test-models
set -euo pipefail

base="https://raw.githubusercontent.com/alecjacobson/common-3d-test-models/master/data"
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for m in cow spot fandisk cheburashka armadillo; do
    echo "fetching $m.obj"
    curl -fsSL -o "$dir/$m.obj" "$base/$m.obj"
done
echo "done -> $dir"

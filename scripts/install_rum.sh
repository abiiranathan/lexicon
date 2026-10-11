#!/usr/bin/env bash

set -euo pipefail

DATABASE=lexicon

# Install RUM extension

# Clone repo if not already present
[ -d rum ] || git clone https://github.com/postgrespro/rum

cd rum
make USE_PGXS=1
sudo make USE_PGXS=1 install

psql -U postgres $DATABASE -c "CREATE EXTENSION IF NOT EXISTS rum;"

cd ..
rm -rf rum

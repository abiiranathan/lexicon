#!/usr/bin/env bash

set -euo pipefail

DATABASE=pdfsearch

git clone https://github.com/postgrespro/rum
cd rum
make USE_PGXS=1
sudo make USE_PGXS=1 install

psql -U postgres $DATABASE -c "CREATE EXTENSION IF NOT EXISTS rum;"

cd ..
rm -rf rum

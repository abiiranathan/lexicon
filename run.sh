#/usr/bin/env bash


./build/pdfsearch --port 3000 --addr localhost -c "postgres://postgres@localhost/pdfsearch"

# find . -type d \( -name node_modules -o -name .cache -o -name build -o -name .git \) -prune -o \
#     -type f \
#     -not -name '.env*' \
#     -not -name 'a.out' \
#     -not -regex '.*\.\(jpg\|jpeg\|png\|gif\|svg\|ico\)$' \
#     -exec sh -c 'echo "### FILE: $1 ###"; cat "$1"' _ {} \;  > /tmp/combined_source.txt
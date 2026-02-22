#!/bin/bash
# Source the IDF environment (normally done by the base image's entrypoint)
# shellcheck disable=SC1091
source /opt/esp/idf/export.sh > /dev/null 2>&1 || true

exec "$@"

#!/usr/bin/env bash
# Host tests (plain Linux, g++ only): progress model + end-to-end Minecraft client <-> GTA bridge.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Wno-unused-function -I../mod/src test_logic.cpp -o /tmp/test_logic
g++ -std=c++17 -Wall -Wextra -Wno-unused-function -pthread -I../mod/src test_bridge_e2e.cpp -o /tmp/test_bridge_e2e
/tmp/test_logic
/tmp/test_bridge_e2e

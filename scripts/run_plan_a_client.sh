#!/usr/bin/env bash
# Plan-A scan, CLIENT (sender) side companion to run_plan_a.sh.
#
# This script just resizes config/send_config to match the total ws count T
# the server side is testing, and launches axio normally (one ws per core,
# i.e. AXIO_QP_PER_CORE=1). The point is that sender should be as fast as
# possible, NOT itself stacking multi-QP-per-core.
#
# Coordination with the receiver:
#   - You must run this with the SAME ordered T list as the server.
#   - Easiest way: run this loop manually one T at a time, after the server
#     has settled on the same T. Or use SSH to step through together.
#   - For a one-shot run on a fixed T, pass --total / -T:
#       ./scripts/run_plan_a_client.sh -T 32
#
# Build prerequisites on this machine:
#   src/common.h:  #define NODE_TYPE CLIENT
#   then `cd build && ninja` to produce build/axio (CLIENT-mode).
#
# Usage:
#   ./scripts/run_plan_a_client.sh -T <total_ws> [results_dir]
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

T=""
RESULT_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -T|--total) T="$2"; shift 2;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \?//'; exit 0;;
    *) RESULT_DIR="$1"; shift;;
  esac
done
[[ -n "${T}" ]] || { echo "usage: $0 -T <total_ws> [results_dir]"; exit 1; }

RESULT_DIR="${RESULT_DIR:-${ROOT}/results/plan_a_client_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "${RESULT_DIR}"

CONFIG_TPL="${ROOT}/config/send_config"
CONFIG_RUN="${ROOT}/config/send_config"
BACKUP="${ROOT}/config/send_config.bak.$$"
cp "${CONFIG_TPL}" "${BACKUP}"
trap 'mv -f "${BACKUP}" "${CONFIG_TPL}"' EXIT

BIN="${ROOT}/build/axio"
[[ -x "${BIN}" ]] || { echo "binary ${BIN} not found, please build first"; exit 1; }

SUDO="${SUDO:-1}"
if [[ "${SUDO}" == "1" ]]; then
  RUN_PREFIX=(sudo -E env)
else
  RUN_PREFIX=(env)
fi

LOG="${RESULT_DIR}/T${T}.log"
echo "==== CLIENT T=${T} ===="

python3 "${ROOT}/scripts/gen_plan_a.py" \
  --total "${T}" --tpl "${BACKUP}" --out "${CONFIG_RUN}" \
  --disp-queue-num "${T}"

# Sender always runs one-ws-per-core (AXIO_QP_PER_CORE=1) for max load gen.
pushd "${ROOT}" >/dev/null
"${RUN_PREFIX[@]}" AXIO_QP_PER_CORE=1 "${BIN}" 2>&1 | tee "${LOG}"
popd >/dev/null

echo "log: ${LOG}"

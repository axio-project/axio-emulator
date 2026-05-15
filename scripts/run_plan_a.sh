#!/usr/bin/env bash
# Plan-A scan: for each (qp_per_core N, core_count C), set total ws = N*C,
# bind every N consecutive ws to the same core, run, collect pps from the log.
#
# Prereqs:
#   - Build the project for the SERVER side (NODE_TYPE=SERVER).
#   - Peer (client) is already running with a matching config, OR provide
#     PEER_HOST + PEER_DIR so that this script can ssh-launch it for each T.
#   - kWorkspaceMaxNum / kMaxQueuesPerPort are large enough for max(N*C).
#
# Env knobs:
#   SUDO=1            run axio with sudo (default: 1)
#   PEER_HOST=user@ip share-the-T client over ssh (optional; if unset you
#                     must keep the client running by hand)
#   PEER_DIR=/path/to/axio-emulator   (required when PEER_HOST set)
#   PEER_SUDO=1       sudo on client side (default: 1)
#
# Usage:
#   ./scripts/run_plan_a.sh [results_dir]

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULT_DIR="${1:-${ROOT}/results/plan_a_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "${RESULT_DIR}"

CONFIG_TPL="${ROOT}/config/recv_config"
CONFIG_RUN="${ROOT}/config/recv_config"   # in-place; we keep a backup
BACKUP="${ROOT}/config/recv_config.bak.$$"
cp "${CONFIG_TPL}" "${BACKUP}"
trap 'mv -f "${BACKUP}" "${CONFIG_TPL}"; cleanup_peer' EXIT

BIN="${ROOT}/build/axio"
[[ -x "${BIN}" ]] || { echo "binary ${BIN} not found, please build first"; exit 1; }

# Run binary as root (DPDK usually needs it for hugepages / vfio).
SUDO="${SUDO:-1}"
if [[ "${SUDO}" == "1" ]]; then
  RUN_PREFIX=(sudo -E env)
else
  RUN_PREFIX=(env)
fi

# --- optional peer (client) auto-launch over ssh ---
PEER_HOST="${PEER_HOST:-}"
PEER_DIR="${PEER_DIR:-}"
PEER_SUDO="${PEER_SUDO:-1}"

start_peer() {
  local t="$1"
  [[ -z "${PEER_HOST}" ]] && return 0
  [[ -n "${PEER_DIR}"  ]] || { echo "PEER_DIR required when PEER_HOST set"; exit 1; }
  echo "[peer] ssh ${PEER_HOST}: launch client with T=${t}"
  ssh -n -f "${PEER_HOST}" \
    "cd '${PEER_DIR}' && \
     SUDO=${PEER_SUDO} nohup ./scripts/run_plan_a_client.sh -T ${t} \
       >/tmp/axio_client_T${t}.log 2>&1 &"
  sleep 5  # give client time to spin up DPDK and start sending
}

stop_peer() {
  [[ -z "${PEER_HOST}" ]] && return 0
  echo "[peer] ssh ${PEER_HOST}: kill axio"
  ssh -n "${PEER_HOST}" "sudo pkill -INT -f '${PEER_DIR:-axio-emulator}.*build/axio' || true"
  sleep 2
}

cleanup_peer() {
  stop_peer || true
}

# ------- test matrix (cap N*C <= kWorkspaceMaxNum, here = 128) -------
declare -a Ns=(8 16 32 64)
declare -a Cs=(1 2 4 8 12 16)

CSV="${RESULT_DIR}/result.csv"
echo "qp_per_core,cores,total_ws,nic_rx_mpps,nic_tx_mpps,log" > "${CSV}"

for N in "${Ns[@]}"; do
  for C in "${Cs[@]}"; do
    T=$(( N * C ))
    if (( T > 128 )); then
      echo "skip N=${N} C=${C} (T=${T} > 128)"
      continue
    fi

    LOG="${RESULT_DIR}/N${N}_C${C}_T${T}.log"
    echo "==== N=${N} C=${C} T=${T} ===="

    # 1) regenerate workload line + bump kDispQueueNum to T
    python3 "${ROOT}/scripts/gen_plan_a.py" \
      --total "${T}" --tpl "${BACKUP}" --out "${CONFIG_RUN}" \
      --disp-queue-num "${T}"

    # 2) start peer (optional)
    start_peer "${T}"

    # 3) run server with AXIO_QP_PER_CORE=N
    pushd "${ROOT}" >/dev/null
    "${RUN_PREFIX[@]}" AXIO_QP_PER_CORE="${N}" "${BIN}" 2>&1 | tee "${LOG}" || true
    popd >/dev/null

    # 4) stop peer
    stop_peer

    # 5) extract aggregated Mpps from log.
    RX=$(grep -E '^\s*nic_rx\b' "${LOG}" | tail -n1 | awk '{print $2}')
    TX=$(grep -E '^\s*nic_tx\b' "${LOG}" | tail -n1 | awk '{print $2}')
    echo "${N},${C},${T},${RX:-NA},${TX:-NA},$(basename "${LOG}")" >> "${CSV}"
    echo "  -> nic_rx=${RX:-NA} Mpps  nic_tx=${TX:-NA} Mpps"

    sleep 2
  done
done

echo "All done. CSV: ${CSV}"

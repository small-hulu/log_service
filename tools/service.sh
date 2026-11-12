#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary="$project/build/log_service"
pidfile="$project/runtime/service.pid"
mkdir -p "$project/runtime"
owned_pid() {
  [[ -f "$pidfile" ]] || return 1
  local candidate
  candidate=$(cat "$pidfile")
  [[ "$candidate" =~ ^[0-9]+$ ]] || return 1
  local executable
  executable=$(readlink "/proc/$candidate/exe" 2>/dev/null || true)
  [[ "$executable" == "$binary" || "$executable" == "$binary (deleted)" ]] || return 1
  printf '%s' "$candidate"
}
case "${1:-status}" in
  start)
    if pid=$(owned_pid); then echo "running pid=$pid"; exit 0; fi
    nohup "$binary" "${2:-$project/config/service.json}" >>"$project/runtime/service.log" 2>&1 </dev/null &
    pid=$!
    printf '%s\n' "$pid" >"$pidfile"
    sleep 1
    kill -0 "$pid" || { cat "$project/runtime/service.log"; exit 1; }
    echo "started pid=$pid"
    ;;
  stop)
    if pid=$(owned_pid); then
      kill -TERM "$pid"
      for attempt in {1..100}; do
        if [[ ! -e "/proc/$pid/exe" ]]; then rm -f "$pidfile"; echo stopped; exit 0; fi
        sleep 0.1
      done
      echo "shutdown still pending pid=$pid"; exit 1
    fi
    echo "not running"
    ;;
  status)
    if pid=$(owned_pid); then echo "running pid=$pid"; else echo "not running"; exit 1; fi
    ;;
  *) echo "usage: $0 start|stop|status"; exit 2 ;;
esac

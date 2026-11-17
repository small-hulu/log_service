#!/usr/bin/env bash
set -euo pipefail

project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
action=${1:-start}
if [[ $# -gt 1 ]]; then
  echo "Usage: bash run.sh [start|stop|restart|status|logs]" >&2
  exit 2
fi

# Use the installed supervisor only when it manages this project directory.
if command -v systemctl >/dev/null 2>&1 &&
   [[ "$(systemctl show log_service.service -p WorkingDirectory --value 2>/dev/null || true)" == "$project" ]]; then
  case "$action" in
    start|stop|restart|status) exec systemctl "$action" log_service.service ;;
  esac
fi

case "$action" in
  start|restart)
    if [[ ! -x "$project/build/log_service" ]]; then
      echo "Service binary missing. Run: bash build.sh" >&2
      exit 1
    fi
    if [[ "$action" == restart ]]; then
      bash "$project/tools/service.sh" stop
    fi
    bash "$project/tools/service.sh" start
    ;;
  stop|status)
    bash "$project/tools/service.sh" "$action"
    ;;
  logs)
    tail -n 50 -F "$project/runtime/service.log"
    ;;
  *)
    echo "Usage: bash run.sh [start|stop|restart|status|logs]" >&2
    exit 2
    ;;
esac

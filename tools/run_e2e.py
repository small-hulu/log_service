"""Run on Windows against the RK fixture config and save verifiable evidence."""
import json
from pathlib import Path
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
output = Path("D:/codex_log/log-service-received")
report = []
cases = [
    ("ALL retained files", ["--mode", "all", "--modules", "hand", "glove", "--verify-fixtures"]),
    ("LATEST includes rotation", ["--mode", "latest", "--modules", "hand", "glove", "--verify-fixtures"]),
    ("selected module", ["--mode", "latest", "--modules", "hand", "--verify-fixtures"]),
    ("empty scope", ["--modules", "empty", "--expect-error", "NO_LOGS"]),
    ("missing startup marker", ["--mode", "latest", "--modules", "legacy", "--expect-error", "LATEST_UNAVAILABLE"]),
    ("unknown module", ["--modules", "unknown", "--expect-error", "UNKNOWN_MODULE"]),
    ("missing ACK", ["--modules", "hand", "--no-ack", "--expect-error", "ACK_TIMEOUT"]),
    ("recovery after timeout", ["--modules", "hand", "--verify-fixtures"]),
]
for name, options in cases:
    result = subprocess.run([sys.executable, str(root / "tools/sdk_client.py"), "--output", str(output), *options],
                            capture_output=True, text=True, timeout=150)
    evidence = json.loads(result.stdout) if result.returncode == 0 else {"passed": False, "error": result.stderr}
    evidence["case"] = name
    report.append(evidence)
    print(name, "PASS" if evidence["passed"] else "FAIL", flush=True)
    time.sleep(0.1)
target = root / "docs/e2e-results.json"
target.parent.mkdir(exist_ok=True)
target.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
print(target)
raise SystemExit(0 if all(x["passed"] for x in report) else 1)

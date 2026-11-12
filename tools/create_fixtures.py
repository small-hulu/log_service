"""Synthetic logs only. Never modifies business module logs."""
from pathlib import Path
import random

root = Path(__file__).resolve().parents[1] / "runtime" / "fixtures"
for module in ("hand", "glove"):
    for run in ("run_old", "run_current"):
        directory = root / module / run
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "app.log").write_text(f"{module} {run} log\n" * 100, encoding="utf-8")
    (root / module / "current_run").write_text("run_current\n", encoding="utf-8")
    # Incompressible deterministic fixture forces multiple transport chunks.
    rng = random.Random(module)
    (root / module / "run_current" / "app.1.log").write_bytes(rng.randbytes(300_000))
(root / "empty").mkdir(parents=True, exist_ok=True)
(root / "legacy").mkdir(parents=True, exist_ok=True)
(root / "legacy" / "legacy.log").write_text("no run marker\n", encoding="utf-8")
print(root)

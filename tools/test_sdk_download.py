"""Exercise the unmodified SDK Logs implementation against a live log_service.

This test creates a PCU fixture like the SDK's own log tests. It deliberately
does not claim to validate discovery/state or the public PokeClient connection.
Requires Python 3.14 and the SDK's Python dependencies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, required=True, help="SDK checkout containing python_sdk")
    parser.add_argument("--endpoint", default="tcp/10.23.2.11:7447")
    parser.add_argument("--pcu", default="rk3588-log-test")
    parser.add_argument("--output", type=Path, default=Path("D:/log_client_test/sdk_protocol_test"))
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--sources", nargs="+", default=["poke_recorder", "video_manager", "pico_broker", "dex_hand"])
    parser.add_argument("--latest-available", action="store_true")
    parser.add_argument("--empty-sources", nargs="*", default=[], help="configured sources containing no files")
    args = parser.parse_args()
    sys.path.insert(0, str(args.sdk_root.resolve() / "python_sdk"))
    import zenoh
    from poke_hand_sdk import LogMode, PokeClient
    from poke_hand_sdk.exceptions import LogDownloadError
    from poke_hand_sdk.pcu import PCU, _ClientSession
    from poke_hand_sdk.types import PCUInfo

    config = zenoh.Config.from_json5(json.dumps({"mode": "client", "connect": {"endpoints": [args.endpoint]},
                                               "scouting": {"multicast": {"enabled": False}}}))
    records = []
    args.output.mkdir(parents=True, exist_ok=True)
    with zenoh.open(config) as session:
        pcu = PCU(args.pcu, _ClientSession(session), PCUInfo(args.pcu, "", "", "", ()))
        try:
            for index in range(args.rounds):
                selected = () if index % 2 == 0 else tuple(args.sources[::2])
                started = time.monotonic()
                result = pcu.logs.download(output_dir=args.output, mode=LogMode.ALL, modules=selected, timeout_ms=30000)
                assert result.path.stat().st_size == result.file_size
                assert hashlib.sha256(result.path.read_bytes()).hexdigest() == result.sha256
                with tarfile.open(result.path) as archive:
                    manifest = json.load(archive.extractfile("manifest.json"))
                    files = manifest["files"]
                    expected_sources = set(selected or args.sources) - set(args.empty_sources)
                    assert {entry["path"].split("/")[0] for entry in files} == expected_sources
                    assert set(archive.getnames()) == {entry["path"] for entry in files} | {"manifest.json"}
                    assert all(archive.getmember(entry["path"]).size == entry["size"] for entry in files)
                records.append({"case": "ALL", "modules": selected, "request_id": result.request_id,
                                "bytes": result.file_size, "sha256": result.sha256, "files": len(files),
                                "seconds": round(time.monotonic() - started, 3), "archive": str(result.path)})
                print(f"PASS SDK download {index + 1}: {len(files)} files, {result.file_size} bytes", flush=True)
            cases = [(LogMode.ALL, ("not_configured",), "UNKNOWN_MODULE")]
            cases.extend((LogMode.ALL, (name,), "NO_LOGS") for name in args.empty_sources)
            if not args.latest_available:
                cases.append((LogMode.LATEST, (args.sources[0],), "LATEST_UNAVAILABLE"))
            for mode, modules, expected in cases:
                try:
                    pcu.logs.download(output_dir=args.output, mode=mode, modules=modules, timeout_ms=10000)
                except LogDownloadError as exc:
                    assert exc.code == expected, str(exc)
                    assert not exc.acknowledgement_sent
                    records.append({"case": expected, "request_id": exc.request_id})
                    print(f"PASS SDK error: {expected}", flush=True)
                else:
                    raise AssertionError(f"expected {expected}")
            if args.latest_available:
                result = pcu.logs.download(output_dir=args.output, mode=LogMode.LATEST,
                                           modules=args.sources[:1], timeout_ms=30000)
                with tarfile.open(result.path) as archive:
                    names = archive.getnames()
                    assert not any("run_old/" in name for name in names)
                    assert any("app.1.log" in name for name in names)
                records.append({"case": "LATEST", "request_id": result.request_id, "bytes": result.file_size})
        finally:
            pcu.close()
    # Read-only public API check, independent from the direct log transfer fixture.
    client = PokeClient(config)
    try:
        discovered = [info.pcu_id for info in client.discover(timeout_ms=1500)]
    finally:
        client.close()
    report = {"passed": True, "endpoint": args.endpoint, "pcu_id": args.pcu,
              "scope": "real SDK PCU.logs with test-only PCU fixture; discovery/state not synthesized",
              "discovered_pcus": discovered, "cases": records}
    (args.output / "sdk-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": True, "cases": len(records), "discovered_pcus": discovered}))


if __name__ == "__main__":
    main()

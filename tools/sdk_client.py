"""Zenoh SDK simulator. Board runtime is C++17; this is a PC test tool."""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import secrets
import sys
import tarfile
import time
import uuid

import flatbuffers
import zenoh

sys.path.insert(0, str(Path(__file__).resolve().parent / "generated"))
from poke.logwire import Message as Wire
from poke.protocol import MessageHeader as Header

GET, BEGIN, CHUNK, END, ACK, COMPLETE, ERROR = range(7)
PRODUCER = "sdk-test-" + uuid.uuid4().hex
EPOCH = secrets.randbits(64) or 1


def encode(kind, request_id, mode=0, modules=(), size=0, digest="", *, sequence=None,
           producer=PRODUCER, epoch=EPOCH, include_header=True):
    b = flatbuffers.Builder(512)
    rid = b.CreateString(request_id)
    sha = b.CreateString(digest)
    names = [b.CreateString(x) for x in modules]
    Wire.MessageStartModulesVector(b, len(names))
    for name in reversed(names):
        b.PrependUOffsetTRelative(name)
    names = b.EndVector()
    producer_offset = b.CreateString(producer)
    Header.Start(b)
    Header.AddProducerId(b, producer_offset)
    Header.AddStreamEpoch(b, epoch)
    Header.AddSequence(b, sequence if sequence is not None else (1 if kind == GET else 2))
    Header.AddTimestamp(b, time.time_ns())
    Header.AddClockDomain(b, 1)
    header = Header.End(b)
    Wire.MessageStart(b)
    if include_header:
        Wire.MessageAddHeader(b, header)
    Wire.MessageAddKind(b, kind)
    Wire.MessageAddRequestId(b, rid)
    Wire.MessageAddMode(b, mode)
    Wire.MessageAddModules(b, names)
    Wire.MessageAddFileSize(b, size)
    Wire.MessageAddSha256(b, sha)
    b.Finish(Wire.MessageEnd(b), file_identifier=b"PLOG")
    return bytes(b.Output())


def text(value):
    return value.decode("utf-8") if value else ""


def download(args):
    rid = args.request_id or uuid.uuid4().hex
    prefix = f"poke/pcu/{args.pcu}/logs/"
    inbox = queue.Queue(maxsize=256)
    overflow = []

    def receive(sample):
        try:
            inbox.put_nowait(bytes(sample.payload))
        except queue.Full:
            overflow.append(True)

    config = zenoh.Config()
    config.insert_json5("mode", '"client"')
    config.insert_json5("connect/endpoints", json.dumps([args.endpoint]))
    config.insert_json5("scouting/multicast/enabled", "false")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    partial = output / f"{rid}.part"
    destination = output / f"logs-{rid}.tar.gz"
    if partial.exists() or destination.exists():
        raise RuntimeError("output already exists; use a fresh request ID or output directory")
    started = time.monotonic()
    metadata = None
    received = set()
    end_received = False
    acked = False
    members = []
    digest = ""
    server_epoch = None
    server_sequence = 0
    archive_name = None
    try:
        with zenoh.open(config) as session:
            subscriber = session.declare_subscriber(prefix + "result/" + rid, receive)
            publisher = session.declare_publisher(prefix + "get", congestion_control=zenoh.CongestionControl.BLOCK)
            deadline = time.monotonic() + 10
            while not publisher.matching_status.matching:
                if time.monotonic() > deadline:
                    raise TimeoutError("no log service GET subscriber")
                time.sleep(0.05)
            publisher.put(encode(GET, rid, 0 if args.mode == "all" else 1, args.modules))
            with partial.open("xb+") as stream:
                while time.monotonic() - started < args.timeout:
                    if overflow:
                        raise RuntimeError("receive queue overflow; reduce board transfer rate")
                    try:
                        raw = inbox.get(timeout=0.2)
                    except queue.Empty:
                        continue
                    if len(raw) < 8 or raw[4:8] != b"PLOG":
                        raise RuntimeError("invalid file identifier")
                    m = Wire.Message.GetRootAsMessage(raw, 0)
                    if m.Version() != 1 or text(m.RequestId()) != rid:
                        raise RuntimeError("response identity/version mismatch")
                    header = m.Header()
                    if (header is None or header.ProtocolMajor() != 2 or header.ProtocolMinor() != 0
                            or text(header.ProducerId()) != args.pcu or not header.StreamEpoch()
                            or header.Sequence() <= server_sequence):
                        raise RuntimeError("invalid or replayed service header")
                    if server_epoch is not None and server_epoch != header.StreamEpoch():
                        raise RuntimeError("service epoch changed during transfer")
                    server_epoch, server_sequence = header.StreamEpoch(), header.Sequence()
                    if m.Kind() == ERROR:
                        code = text(m.ErrorCode())
                        if args.expect_error == code:
                            return {"passed": True, "expected_error": code, "request_id": rid}
                        raise RuntimeError(f"{code}: {text(m.Detail())}")
                    if m.Kind() == BEGIN:
                        archive_name = text(m.FileName())
                        candidate = (m.FileSize(), m.ChunkSize(), m.ChunkCount(), text(m.Sha256()))
                        size, chunk_size, count, sha = candidate
                        if not (0 < size <= 512 * 1024 * 1024 and 1024 <= chunk_size <= 65536
                                and count == (size + chunk_size - 1) // chunk_size and len(sha) == 64):
                            raise RuntimeError("invalid archive metadata")
                        if metadata is not None and metadata != candidate:
                            raise RuntimeError("metadata changed during transfer")
                        metadata = candidate
                    elif m.Kind() == CHUNK:
                        if metadata is None:
                            raise RuntimeError("chunk before metadata")
                        size, chunk_size, count, _ = metadata
                        index = m.ChunkIndex()
                        expected = min(chunk_size, size - index * chunk_size)
                        if index >= count or m.DataLength() != expected:
                            raise RuntimeError("invalid chunk index or size")
                        # Schema v1 data field (slot 11 / vtable offset 26).
                        # Slice its byte vector to avoid a Python call per byte.
                        start = m._tab.Vector(m._tab.Offset(26))
                        data = bytes(raw[start:start + m.DataLength()])
                        if len(data) != expected:
                            raise RuntimeError("truncated data vector")
                        stream.seek(index * chunk_size)
                        if index in received:
                            if stream.read(len(data)) != data:
                                raise RuntimeError("conflicting duplicate chunk")
                        else:
                            stream.write(data)
                            received.add(index)
                    elif m.Kind() == END:
                        if (archive_name != text(m.FileName()) or
                                metadata != (m.FileSize(), m.ChunkSize(), m.ChunkCount(), text(m.Sha256()))):
                            raise RuntimeError("END metadata mismatch")
                        end_received = True
                    elif m.Kind() == COMPLETE:
                        if not acked:
                            raise RuntimeError("completion before verified ACK")
                        if (archive_name != text(m.FileName()) or
                                metadata != (m.FileSize(), m.ChunkSize(), m.ChunkCount(), text(m.Sha256()))):
                            raise RuntimeError("COMPLETE metadata mismatch")
                        break
                    if metadata and end_received and len(received) == metadata[2] and not acked:
                        stream.flush()
                        stream.seek(0)
                        h = hashlib.sha256()
                        while block := stream.read(65536):
                            h.update(block)
                        digest = h.hexdigest()
                        if digest != metadata[3] or partial.stat().st_size != metadata[0]:
                            raise RuntimeError("archive hash/size mismatch")
                        with tarfile.open(partial, "r:gz") as archive:
                            members = archive.getnames()
                            manifest = json.load(archive.extractfile("manifest.json"))
                            logs = {item["path"] for item in manifest["files"]}
                            if set(members) != logs | {"manifest.json"}:
                                raise RuntimeError("archive does not match manifest")
                            for item in manifest["files"]:
                                if archive.getmember(item["path"]).size != item["size"]:
                                    raise RuntimeError("manifest file size mismatch")
                            if args.verify_fixtures:
                                if args.mode == "latest" and any("run_old/" in x for x in members):
                                    raise RuntimeError("LATEST contains an old run")
                                if args.mode == "latest" and not any("app.1.log" in x for x in members):
                                    raise RuntimeError("LATEST missing rotated files")
                                if args.mode == "all" and not any("run_old/" in x for x in members):
                                    raise RuntimeError("ALL missing historical files")
                        if not args.no_ack:
                            session.put(prefix + "ack", encode(ACK, rid, size=metadata[0], digest=digest))
                            acked = True
                else:
                    raise TimeoutError("no complete confirmed transfer before deadline")
            subscriber.undeclare()
            publisher.undeclare()
        if args.expect_error:
            raise RuntimeError("expected error was not returned")
        partial.replace(destination)
        return {"passed": True, "mode": args.mode, "request_id": rid, "archive": str(destination),
                "bytes": metadata[0], "chunks": len(received), "sha256": digest,
                "server_epoch": server_epoch, "last_sequence": server_sequence,
                "members": members, "elapsed_seconds": round(time.monotonic() - started, 3)}
    finally:
        partial.unlink(missing_ok=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--endpoint", default="tcp/10.23.2.11:7447")
    p.add_argument("--pcu", default="rk3588-log-test")
    p.add_argument("--mode", choices=["all", "latest"], default="all")
    p.add_argument("--modules", nargs="*", default=[])
    p.add_argument("--request-id")
    p.add_argument("--output", default="D:/log_client_test")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--verify-fixtures", action="store_true")
    p.add_argument("--expect-error")
    p.add_argument("--no-ack", action="store_true")
    args = p.parse_args()
    try:
        print(json.dumps(download(args), ensure_ascii=False, indent=2))
    except Exception as exc:
        print(json.dumps({"passed": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

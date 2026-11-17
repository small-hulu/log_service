"""Live PLOG v1/common Header v2 regression checks; no source logs are modified."""
import argparse
from collections import defaultdict, deque
import json
from pathlib import Path
import queue
import time
import uuid

import zenoh
from sdk_client import ACK, BEGIN, COMPLETE, END, ERROR, GET, EPOCH, PRODUCER, Wire, encode, text


class Probe:
    def __init__(self, session, pcu):
        self.session, self.pcu = session, pcu
        self.prefix = f"poke/pcu/{pcu}/logs/"
        self.inbox = queue.Queue(maxsize=2048)
        self.pending = defaultdict(deque)
        self.headers = {}
        self.frames = 0
        self.subscriber = session.declare_subscriber(self.prefix + "result/*", lambda s: self.inbox.put_nowait(bytes(s.payload)))
        self.publisher = session.declare_publisher(self.prefix + "get")
        deadline = time.monotonic() + 10
        while not self.publisher.matching_status.matching:
            if time.monotonic() > deadline:
                raise TimeoutError("no GET subscriber")
            time.sleep(0.02)

    def next(self, rid, timeout):
        deadline = time.monotonic() + timeout
        while not self.pending[rid]:
            raw = self.inbox.get(timeout=max(0, deadline - time.monotonic()))
            assert raw[4:8] == b"PLOG"
            m = Wire.Message.GetRootAsMessage(raw, 0)
            h = m.Header()
            request_id = text(m.RequestId())
            assert m.Version() == 1 and h and h.ProtocolMajor() == 2 and h.ProtocolMinor() == 0
            assert text(h.ProducerId()) == self.pcu and h.StreamEpoch() and h.Sequence()
            assert h.Timestamp() and h.ClockDomain() == 1 and h.TimestampUnit() == 0
            previous = self.headers.get(request_id)
            assert previous is None or (h.StreamEpoch() == previous[0] and h.Sequence() > previous[1])
            self.headers[request_id] = (h.StreamEpoch(), h.Sequence())
            self.frames += 1
            self.pending[request_id].append(m)
        return self.pending[rid].popleft()

    def wait(self, rid, kind, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            m = self.next(rid, max(0, deadline - time.monotonic()))
            if m.Kind() == kind:
                return m
            assert m.Kind() != ERROR, f"unexpected error: {text(m.ErrorCode())}"

    def silent(self, rid, timeout=0.15):
        try:
            m = self.next(rid, timeout)
        except queue.Empty:
            return
        raise AssertionError(f"unexpected response kind={m.Kind()} for {rid}")

    def get(self, rid=None, **kwargs):
        rid = rid or uuid.uuid4().hex
        self.publisher.put(encode(GET, rid, **kwargs))
        return rid

    def ack(self, rid, metadata, **kwargs):
        self.session.put(self.prefix + "ack", encode(ACK, rid, size=kwargs.pop("size", metadata.FileSize()),
                         digest=kwargs.pop("digest", text(metadata.Sha256())), **kwargs))

    def close(self):
        self.subscriber.undeclare()
        self.publisher.undeclare()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--endpoint", default="tcp/10.23.2.11:17447")
    p.add_argument("--pcu", default="rk3588-log-test")
    p.add_argument("--ack-timeout", type=float, default=5)
    p.add_argument("--report", type=Path, required=True)
    args = p.parse_args()
    cases = []

    def passed(name):
        cases.append(name)
        print("PASS " + name, flush=True)

    config = zenoh.Config.from_json5(json.dumps({"mode": "client", "connect": {"endpoints": [args.endpoint]},
                                               "scouting": {"multicast": {"enabled": False}}}))
    with zenoh.open(config) as session:
        probe = Probe(session, args.pcu)
        try:
            for label, kwargs in [("missing_header", {"include_header": False}), ("zero_epoch", {"epoch": 0}),
                                  ("zero_sequence", {"sequence": 0}), ("empty_producer", {"producer": ""})]:
                rid = probe.get(**kwargs); probe.silent(rid, 0.25); passed(label)
            rid = probe.get(); end = probe.wait(rid, END)
            probe.get(rid); probe.silent(rid); passed("active_duplicate_ignored")
            busy = probe.get(); error = probe.wait(busy, ERROR)
            assert text(error.ErrorCode()) == "BUSY"; passed("concurrent_BUSY_with_header")
            invalid_acks = [
                ("wrong_producer", {"producer": PRODUCER + "-other", "sequence": 100}),
                ("wrong_epoch", {"epoch": EPOCH ^ 1 or 2, "sequence": 100}),
                ("old_sequence", {"sequence": 1}),
                ("wrong_hash", {"digest": "0" * 64, "sequence": 2}),
                ("replayed_corrected_ack", {"sequence": 2}),
                ("wrong_size", {"size": end.FileSize() + 1, "sequence": 3}),
            ]
            for label, kwargs in invalid_acks:
                probe.ack(rid, end, **kwargs); probe.silent(rid); passed(label)
            probe.get(rid, sequence=4); probe.silent(rid)
            probe.ack(rid, end, sequence=4); probe.silent(rid); passed("duplicate_GET_advances_client_sequence")
            probe.ack(rid, end, sequence=5)
            complete = probe.wait(rid, COMPLETE)
            assert complete.FileSize() == end.FileSize() and text(complete.Sha256()) == text(end.Sha256())
            passed("fresh_valid_ACK_completes")
            time.sleep(0.05)
            probe.get(rid, sequence=6)
            error = probe.wait(rid, ERROR); assert text(error.ErrorCode()) == "DUPLICATE"
            passed("finished_request_duplicate")
            rid = probe.get(); end = probe.wait(rid, END); started = time.monotonic()
            while True:
                probe.ack(rid, end, producer="wrong-client", sequence=100)
                try:
                    m = probe.next(rid, 0.2)
                except queue.Empty:
                    assert time.monotonic() - started < args.ack_timeout + 2, "invalid ACK extended deadline"
                    continue
                assert m.Kind() == ERROR and text(m.ErrorCode()) == "ACK_TIMEOUT"
                break
            elapsed = time.monotonic() - started
            assert args.ack_timeout - 1 <= elapsed <= args.ack_timeout + 2
            passed("invalid_ACKs_do_not_extend_timeout")
            time.sleep(0.05)
            rid = probe.get(); end = probe.wait(rid, END)
            probe.ack(rid, end); probe.wait(rid, COMPLETE); passed("service_recovers_after_timeout")
            report = {"passed": True, "endpoint": args.endpoint, "cases": cases,
                      "frames_checked": probe.frames, "headers": probe.headers, "timeout_seconds": round(elapsed, 3)}
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
        finally:
            probe.close()


if __name__ == "__main__":
    main()

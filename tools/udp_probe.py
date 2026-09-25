#!/usr/bin/env python3
"""Bounded UDP load/RTT probe for an OpenNOW-3DS Gate 1 build."""

import argparse
import random
import socket
import struct
import time

MAGIC = 0x4F4E3344
VERSION = 1
HELLO, LOAD, ECHO, ECHO_REPLY, END, REPORT = range(6)
HEADER = struct.Struct("!IHHIIQHH")
REPORT = struct.Struct("!5I")


def packet(kind, run_id, sequence, payload=b"", stamp=None):
    if stamp is None:
        stamp = time.monotonic_ns()
    return HEADER.pack(MAGIC, VERSION, kind, run_id, sequence, stamp,
                       len(payload), 0) + payload


def unpack(data):
    if len(data) < HEADER.size:
        return None
    values = HEADER.unpack_from(data)
    if values[0] != MAGIC or values[1] != VERSION:
        return None
    if values[6] != len(data) - HEADER.size:
        return None
    return values, data[HEADER.size:]


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def self_test():
    data = packet(LOAD, 7, 11, b"test")
    parsed = unpack(data)
    assert parsed and parsed[0][2:5] == (LOAD, 7, 11)
    assert parsed[1] == b"test"
    assert unpack(data[:-1]) is None
    assert percentile([1, 2, 3, 4], 0.5) == 2
    print("udp_probe self-test passed")


def run(args):
    payload_length = max(0, args.payload - HEADER.size)
    interval = (args.payload * 8) / (args.rate * 1_000_000)
    run_id = random.SystemRandom().randrange(1, 0xFFFFFFFF)
    sequence = 0
    sent_load = 0
    sent_bytes = 0
    late_sends = 0
    rtts = []
    pending = {}
    replies = 0
    echo_sent = 0
    target = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0)
    sock.sendto(packet(HELLO, run_id, sequence), target)
    sequence += 1
    start = time.monotonic()
    next_send = start
    next_echo = start
    try:
        while time.monotonic() - start < args.duration:
            now = time.monotonic()
            if now >= next_send:
                stamp = time.monotonic_ns()
                data = packet(LOAD, run_id, sequence,
                              b"\0" * payload_length, stamp)
                sock.sendto(data, target)
                sent_load += 1
                sent_bytes += len(data)
                sequence += 1
                next_send += interval
                if now - next_send > interval:
                    late_sends += 1
                    next_send = now + interval
            if now >= next_echo:
                stamp = time.monotonic_ns()
                data = packet(ECHO, run_id, sequence, b"", stamp)
                sock.sendto(data, target)
                pending[sequence] = stamp
                echo_sent += 1
                sequence += 1
                next_echo = now + 0.1
            while True:
                try:
                    data, _ = sock.recvfrom(2048)
                except (BlockingIOError, InterruptedError):
                    break
                parsed = unpack(data)
                if not parsed:
                    continue
                header, body = parsed
                if header[2] == ECHO_REPLY and header[3] == run_id:
                    sent_stamp = pending.pop(header[4], header[5])
                    rtts.append((time.monotonic_ns() - sent_stamp) / 1_000_000)
                    replies += 1
                elif header[2] == REPORT and header[3] == run_id and len(body) == REPORT.size:
                    report = REPORT.unpack(body)
                    print("console report: rx_packets=%d rx_bytes=%d lost=%d dup=%d reorder=%d" % report)
            time.sleep(min(0.002, max(0.0002, next_send - time.monotonic())))
    finally:
        sock.sendto(packet(END, run_id, sequence), target)
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            try:
                data, _ = sock.recvfrom(2048)
            except (BlockingIOError, InterruptedError):
                time.sleep(0.01)
                continue
            parsed = unpack(data)
            if parsed and parsed[0][2] == REPORT and len(parsed[1]) == REPORT.size:
                print("console report: rx_packets=%d rx_bytes=%d lost=%d dup=%d reorder=%d" % REPORT.unpack(parsed[1]))
                break
        sock.close()
    print("sent: %d packets, %.3f MiB, requested %.3f Mbit/s" %
          (sent_load, sent_bytes / 1048576, args.rate))
    print("late pacing events: %d; echo replies: %d/%d" % (late_sends, replies, echo_sent))
    if rtts:
        print("RTT ms: p50=%.2f p95=%.2f p99=%.2f" %
              (percentile(rtts, .50), percentile(rtts, .95), percentile(rtts, .99)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host", nargs="?")
    parser.add_argument("--port", type=int, default=50010)
    parser.add_argument("--rate", type=float, default=1.0, help="load in Mbit/s")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--payload", type=int, default=1200, help="datagram size")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.host:
        run(args)
    else:
        parser.error("host is required unless --self-test is used")


if __name__ == "__main__":
    main()

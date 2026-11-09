#!/usr/bin/env python3
"""
Building AP Layer-2 Broadcast & Client-Isolation Commissioning Check (I-9, I-10).

Run in `--listen` mode on one machine connected to the building Wi-Fi AP, and
in `--send` mode on another machine connected to the same AP (or repeater).
Verifies that UDP port 19500 limited broadcast (255.255.255.255) and unicast
replies traverse the AP without client isolation or broadcast filtering.
"""

import argparse
import socket
import sys
import time

DEFAULT_PORT = 19500
PROBE_PAYLOAD = b"DORBELLAP_L2_COMMISSIONING_PROBE_V1"
ACK_PAYLOAD = b"DORBELLAP_L2_COMMISSIONING_ACK_V1"


def run_listener(port: int) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", port))
    print(f"Listening for UDP broadcast probes on 0.0.0.0:{port} ...")
    while True:
        data, addr = sock.recvfrom(256)
        if data == PROBE_PAYLOAD:
            print(f"Received broadcast probe from {addr[0]}:{addr[1]} — sending unicast ACK")
            sock.sendto(ACK_PAYLOAD, addr)


def run_sender(port: int, count: int) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.5)

    success = 0
    for seq in range(1, count + 1):
        t0 = time.monotonic()
        sock.sendto(PROBE_PAYLOAD, ("255.255.255.255", port))
        try:
            data, addr = sock.recvfrom(256)
            if data == ACK_PAYLOAD:
                rtt_ms = (time.monotonic() - t0) * 1000.0
                print(f"[{seq}/{count}] Broadcast -> Unicast ACK from {addr[0]} in {rtt_ms:.1f} ms")
                success += 1
        except socket.timeout:
            print(f"[{seq}/{count}] Timeout (>500 ms) — check AP client isolation / broadcast filter")
        time.sleep(0.2)

    print(f"\nResult: {success}/{count} probes acknowledged.")
    return 0 if success == count else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="DorbellAp Building AP Layer-2 Broadcast Check")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--listen", action="store_true", help="Run as broadcast responder")
    group.add_argument("--send", action="store_true", help="Send broadcast probes and measure RTT")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default {DEFAULT_PORT})")
    parser.add_argument("--count", type=int, default=10, help="Number of probes to send")
    args = parser.parse_args()

    if args.listen:
        return run_listener(args.port)
    return run_sender(args.port, args.count)


if __name__ == "__main__":
    sys.exit(main())


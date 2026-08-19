#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A tiny MQTT 3.1.1 broker for developing against without installing one.

CI runs the interop job against real Mosquitto, and that is the test that
counts. This exists for the other situation: a laptop, a container or a
restricted build agent where `apt install mosquitto` is not on the table, and
you still want `pubsub_demo` or your own port to have something to talk to.

    python3 tools/stub_broker.py --port 1883 &
    ./build/examples/pubsub_demo 127.0.0.1 1883 demo/topic

It speaks enough of the protocol to hold a real session: CONNECT/CONNACK,
PUBLISH in both directions at QoS 0, 1 and 2 with the full acknowledgement
handshakes, SUBSCRIBE/SUBACK, UNSUBSCRIBE/UNSUBACK, PINGREQ/PINGRESP and
DISCONNECT. Messages are routed back to matching subscriptions, so a client
that subscribes to what it publishes sees its own traffic.

The fault-injection switches are the reason to reach for this over a real
broker: refusing a CONNECT with a specific return code, or dropping the socket
mid-session, are awkward to arrange in Mosquitto and are exactly the paths a
client gets wrong.

What it is not:

  * Not a broker. No persistence, no retained messages, no will delivery, no
    authentication, no TLS, one client at a time, and no attempt at the
    concurrency a real broker needs.
  * Not a conformance oracle. It is permissive: it will not catch a client
    sending something the spec forbids. Mosquitto in the interop job is what
    checks that.
  * Not a substitute for the CI interop job.

Requires nothing but the standard library.
"""

import argparse
import socket
import sys

CONNECT, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL, PUBCOMP = 1, 2, 3, 4, 5, 6, 7
SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK = 8, 9, 10, 11
PINGREQ, PINGRESP, DISCONNECT = 12, 13, 14

NAMES = {
    CONNECT: "CONNECT", CONNACK: "CONNACK", PUBLISH: "PUBLISH",
    PUBACK: "PUBACK", PUBREC: "PUBREC", PUBREL: "PUBREL",
    PUBCOMP: "PUBCOMP", SUBSCRIBE: "SUBSCRIBE", SUBACK: "SUBACK",
    UNSUBSCRIBE: "UNSUBSCRIBE", UNSUBACK: "UNSUBACK", PINGREQ: "PINGREQ",
    PINGRESP: "PINGRESP", DISCONNECT: "DISCONNECT",
}


# ---------------------------------------------------------------------------
# Wire helpers
# ---------------------------------------------------------------------------

def encode_len(n):
    """Variable byte integer, MQTT 3.1.1 section 2.2.3."""
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        out.append(byte | (0x80 if n else 0))
        if not n:
            return bytes(out)


def topic_matches(filt, topic):
    """Wildcard matching, MQTT 3.1.1 section 4.7.

    Simplified but not wrong for the cases a test client exercises: '+' is one
    level, '#' is the rest, and a leading '$' is not matched by a filter that
    starts with a wildcard.
    """
    if topic.startswith("$") and filt[:1] in ("+", "#"):
        return False

    f, t = filt.split("/"), topic.split("/")
    for i, seg in enumerate(f):
        if seg == "#":
            return True
        if i >= len(t):
            return False
        if seg != "+" and seg != t[i]:
            return False
    return len(f) == len(t)


class Peer:
    """One connected client, and the session state that goes with it."""

    def __init__(self, sock, out):
        self.sock = sock
        self.out = out
        self.subs = {}          # filter -> granted qos
        self.next_id = 0
        self.pending_qos2 = set()   # ids we sent, awaiting PUBREC
        self.packets = 0

    def say(self, msg):
        print(msg, file=self.out, flush=True)

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def read_packet(self):
        head = self.recv_exact(1)
        if head is None:
            return None
        mult, length = 1, 0
        for _ in range(4):
            b = self.recv_exact(1)
            if b is None:
                return None
            length += (b[0] & 0x7F) * mult
            if not (b[0] & 0x80):
                break
            mult *= 128
        body = self.recv_exact(length) if length else b""
        if body is None:
            return None
        self.packets += 1
        return head[0], body

    def send(self, first, body=b""):
        self.sock.sendall(bytes([first]) + encode_len(len(body)) + body)

    def send_ack(self, ptype, packet_id, flags=0):
        self.send((ptype << 4) | flags, packet_id.to_bytes(2, "big"))
        self.say("Sending %s to client (m%d)" % (NAMES[ptype], packet_id))

    def alloc_id(self):
        self.next_id = self.next_id % 65535 + 1
        return self.next_id


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------

def deliver(peer, topic, payload, published_qos):
    """Route a message back to every matching subscription."""
    for filt, granted in peer.subs.items():
        if not topic_matches(filt, topic):
            continue

        # MQTT-3.8.4-6: delivered QoS is the lesser of published and granted.
        qos = min(published_qos, granted)
        var = len(topic).to_bytes(2, "big") + topic.encode()
        packet_id = 0
        if qos:
            packet_id = peer.alloc_id()
            var += packet_id.to_bytes(2, "big")
            if qos == 2:
                peer.pending_qos2.add(packet_id)

        peer.send((PUBLISH << 4) | (qos << 1), var + payload)
        peer.say("Sending PUBLISH to client (q%d, m%d, '%s')"
                 % (qos, packet_id, topic))


def handle_publish(peer, flags, body):
    qos = (flags >> 1) & 3
    dup = (flags >> 3) & 1
    retain = flags & 1

    tlen = int.from_bytes(body[0:2], "big")
    topic = body[2:2 + tlen].decode("utf-8", "replace")
    off = 2 + tlen

    packet_id = 0
    if qos:
        packet_id = int.from_bytes(body[off:off + 2], "big")
        off += 2
    payload = body[off:]

    peer.say("Received PUBLISH from client (d%d, q%d, r%d, m%d, '%s', %d bytes)"
             % (dup, qos, retain, packet_id, topic, len(payload)))

    if qos == 1:
        peer.send_ack(PUBACK, packet_id)
    elif qos == 2:
        # PUBCOMP follows once the client sends PUBREL.
        peer.send_ack(PUBREC, packet_id)

    deliver(peer, topic, payload, qos)


def handle_subscribe(peer, body):
    packet_id = int.from_bytes(body[0:2], "big")
    granted = []
    off = 2
    while off + 2 <= len(body):
        tlen = int.from_bytes(body[off:off + 2], "big")
        filt = body[off + 2:off + 2 + tlen].decode("utf-8", "replace")
        qos = body[off + 2 + tlen]
        off += 2 + tlen + 1

        peer.subs[filt] = qos
        granted.append(qos)
        peer.say("  subscription '%s' granted qos %d" % (filt, qos))

    peer.send((SUBACK << 4), packet_id.to_bytes(2, "big") + bytes(granted))
    peer.say("Sending SUBACK to client (m%d, %s)"
             % (packet_id, granted))


def handle_unsubscribe(peer, body):
    packet_id = int.from_bytes(body[0:2], "big")
    off = 2
    while off + 2 <= len(body):
        tlen = int.from_bytes(body[off:off + 2], "big")
        filt = body[off + 2:off + 2 + tlen].decode("utf-8", "replace")
        off += 2 + tlen
        if peer.subs.pop(filt, None) is not None:
            peer.say("  removed subscription '%s'" % filt)

    peer.send_ack(UNSUBACK, packet_id)


def serve(peer, args):
    """Run one session. Returns when the client goes away."""
    while True:
        try:
            pkt = peer.read_packet()
        except socket.timeout:
            peer.say("Socket timeout, closing")
            return
        except ConnectionResetError:
            peer.say("Client reset the connection")
            return

        if pkt is None:
            peer.say("Client closed the connection")
            return

        first, body = pkt
        ptype = first >> 4
        flags = first & 0x0F

        if args.drop_after and peer.packets > args.drop_after:
            peer.say("Dropping the connection after %d packets (--drop-after)"
                     % args.drop_after)
            return

        if ptype == PUBLISH:
            handle_publish(peer, flags, body)
            continue

        peer.say("Received %s from client" % NAMES.get(ptype, "UNKNOWN(%d)" % ptype))

        if ptype == CONNECT:
            if args.refuse:
                peer.send(CONNACK << 4, bytes([0, args.refuse]))
                peer.say("Sending CONNACK to client (0, %d) -- refused by "
                         "--refuse" % args.refuse)
                return
            peer.send(CONNACK << 4, bytes([1 if args.session_present else 0, 0]))
            peer.say("Sending CONNACK to client (%d, 0)"
                     % (1 if args.session_present else 0))

        elif ptype == PUBREL:
            peer.send_ack(PUBCOMP, int.from_bytes(body[0:2], "big"))

        elif ptype == PUBREC:
            # Client is acknowledging a QoS 2 message we delivered.
            packet_id = int.from_bytes(body[0:2], "big")
            peer.pending_qos2.discard(packet_id)
            peer.send_ack(PUBREL, packet_id, flags=0x02)

        elif ptype in (PUBACK, PUBCOMP):
            pass   # our outbound message is done with

        elif ptype == SUBSCRIBE:
            handle_subscribe(peer, body)

        elif ptype == UNSUBSCRIBE:
            handle_unsubscribe(peer, body)

        elif ptype == PINGREQ:
            peer.send(PINGRESP << 4)
            peer.say("Sending PINGRESP to client")

        elif ptype == DISCONNECT:
            peer.say("Client disconnected cleanly")
            return


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1",
                   help="address to bind (default: %(default)s)")
    p.add_argument("--port", type=int, default=1883,
                   help="port to bind (default: %(default)s)")
    p.add_argument("--log", default="-",
                   help="log destination, or '-' for stdout (default: -)")
    p.add_argument("--once", action="store_true",
                   help="serve a single session and exit")
    p.add_argument("--timeout", type=float, default=120.0,
                   help="socket timeout in seconds (default: %(default)s)")
    p.add_argument("--session-present", action="store_true",
                   help="report session_present=1 in CONNACK, to exercise the "
                        "client's session-resumption path")
    p.add_argument("--refuse", type=int, metavar="CODE", default=0,
                   help="refuse every CONNECT with this CONNACK return code "
                        "(1..5), to exercise the client's failure path")
    p.add_argument("--drop-after", type=int, metavar="N", default=0,
                   help="close the socket without warning after N packets, to "
                        "exercise reconnection and retransmission")
    args = p.parse_args()

    out = sys.stdout if args.log == "-" else open(args.log, "w")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print("stub broker listening on %s:%d" % (args.host, args.port),
          file=out, flush=True)

    try:
        while True:
            sock, addr = srv.accept()
            sock.settimeout(args.timeout)
            peer = Peer(sock, out)
            peer.say("New connection from %s:%d" % addr)
            try:
                serve(peer, args)
            finally:
                sock.close()
                peer.say("Connection closed")
            if args.once:
                return 0
    except KeyboardInterrupt:
        print("interrupted", file=out, flush=True)
        return 0
    finally:
        srv.close()


if __name__ == "__main__":
    sys.exit(main())

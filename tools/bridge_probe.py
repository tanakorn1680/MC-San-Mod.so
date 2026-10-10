#!/usr/bin/env python3
"""Talk to the MCBridge plugin inside GTA SA (loopback TCP, text lines).

Run it on the SAME phone as the game (Termux: `pkg install python`) while the game is running:
    python bridge_probe.py --send PING --send SPAWN_PED --seconds 5
    python bridge_probe.py --send "SPAWN_OBJ gunbox"
    python bridge_probe.py --fake-mc --seconds 10      # sends fake Minecraft entities near the player (draft E lines)
Prints every line received, then a one-line summary you can paste back.
"""
import argparse, socket, sys, time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=47800)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--send", action="append", default=[], help="line to send after connecting (repeatable)")
    ap.add_argument("--fake-mc", action="store_true", help="send draft E lines orbiting the last known player position")
    a = ap.parse_args()

    try:
        s = socket.create_connection((a.host, a.port), timeout=3)
    except OSError as e:
        print(f"CONNECT FAILED {a.host}:{a.port}: {e}")
        print("SUMMARY connected=0")
        return 1
    s.settimeout(0.2)
    t0 = time.time()
    buf = b""
    got = {"HELLO": 0, "P": 0, "other": 0}
    last_p = None
    replies = []
    for line in a.send:
        s.sendall(line.encode() + b"\n")
        print(f"> {line}")
    fake_id = 0
    while time.time() - t0 < a.seconds:
        try:
            d = s.recv(4096)
            if not d:
                print("server closed the connection")
                break
            buf += d
        except socket.timeout:
            pass
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            txt = ln.decode(errors="replace").strip()
            if txt.startswith("HELLO"): got["HELLO"] += 1
            elif txt.startswith("P "):
                got["P"] += 1; last_p = txt
                if got["P"] <= 3 or got["P"] % 20 == 0: print(f"< {txt}")
            else:
                got["other"] += 1; replies.append(txt); print(f"< {txt}")
        if a.fake_mc and last_p:
            _, _, x, y, z = last_p.split()
            fake_id += 1
            s.sendall(f"E 1 zombie {float(x)+4:.2f} {float(y):.2f} {float(z):.2f} 0\n".encode())
            time.sleep(0.1)
    print(f"SUMMARY connected=1 hello={got['HELLO']} player_msgs={got['P']} other={got['other']} replies={replies} last_player={last_p}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

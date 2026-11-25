#!/usr/bin/env python3
"""Add this loadgen's bots. Args: loadgen index 0-3, then lines of 'sid ip' for every server."""
import socket, sys
idx = int(sys.argv[1])
servers = []
for line in sys.stdin:
    sid, ip = line.split()
    n = int(sid[1:])
    servers.append((n, ip))
mine = [s for s in servers if s[0] % 4 == idx]
s = socket.create_connection(("127.0.0.1", 9100), 10)
nadd = 0
for n, ip in mine:
    for k in range(50):
        name = f"Load{n * 50 + k + 1}"
        line = f"add {name} {ip} 25565 wander 2 8 1200 - /dev/null\n"
        s.sendall(line.encode())
        reply = b""
        while not reply.endswith(b"\n"):
            chunk = s.recv(64)
            if not chunk:
                raise SystemExit(f"closed during {name}")
            reply += chunk
        if not reply.startswith(b"ok"):
            raise SystemExit(reply.decode())
        nadd += 1
print(f"added {nadd} on loadgen {idx}")

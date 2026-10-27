# SPDX-License-Identifier: MIT
# The listener of thread-hub.sh: micropython thread-hub.py PORT LOG
# Each UDP datagram on the port goes to the console and to the log, with the
# time and the address of the sender.
import binascii
import socket
import sys
import time

port, log = int(sys.argv[1]), sys.argv[2]
sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock.bind(socket.getaddrinfo("::", port, socket.AF_INET6)[0][-1])
with open(log, "a") as f:
    while True:
        data, peer = sock.recvfrom(256)
        # The unix port gives the raw sockaddr_in6: the address is at byte 8.
        sender = socket.inet_ntop(socket.AF_INET6, peer[8:24])
        try:
            text = data.decode().strip()
        except UnicodeError:
            text = binascii.hexlify(data).decode()
        line = "%04d-%02d-%02d %02d:%02d:%02d %s %s" % (time.localtime()[:6] + (sender, text))
        print(line)
        f.write(line + "\n")
        f.flush()

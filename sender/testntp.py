import socket
import struct
import sys
import time

def RequestTimefromNtp(addr='45.159.204.28'):
    REF_TIME_1970 = 2208988800  # Reference time
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data = b'\x1b' + 47 * b'\0'
    client.sendto(data, (addr, 123))
    data, address = client.recvfrom(1024)
    if data:
        t = struct.unpack('!12I', data)[10]
        t -= REF_TIME_1970
    return time.ctime(t), t

if __name__ == "__main__":
    while True:
        print(RequestTimefromNtp())
        time.sleep(0.25)
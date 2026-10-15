import sys, time, serial

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 921600
try:
    s = serial.Serial(port, baud, timeout=0.2)
except Exception as e:
    print(f"{port}: OPEN FAILED: {e}")
    sys.exit(0)
buf = bytearray()
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(256)
    if d:
        buf += d
print(f"{port}: rx {len(buf)} bytes")
if buf:
    print(" ".join("%02X" % b for b in buf[:128]))
s.close()

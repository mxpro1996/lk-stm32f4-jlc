#!/usr/bin/env python3
"""Boot an LK uImage on a U-Boot board over a serial console and TFTP.

Waits for U-Boot's autoboot prompt on PORT, breaks in, runs dhcp, fetches FILE from
SERVER with tftpboot, optionally sets /chosen/bootargs in U-Boot's device tree (the way
to hand LK an lk.autorun script), then bootm's the image and keeps logging for --after
seconds. Everything seen on the console goes to LOGFILE.

    scripts/uboot-tftp-boot.py /dev/ttyUSB3 boot.log 192.168.0.4 lk.uimg \
        --bootargs 'lk.autorun=ut+all;poweroff'

--first LINE types LINE before waiting, e.g. 'reboot' at a running LK prompt, so a live
board is cycled without touching it. Characters are sent with a small delay because
U-Boot drops input that arrives faster than it polls the UART. DTR and RTS are left
deasserted since FTDI debug bridges sometimes wire them to a reset.

Needs pyserial. The uImage is made with
    mkimage -A riscv -O linux -T kernel -C none -a LOAD -e LOAD -n lk -d lk.bin lk.uimg
"""
import argparse, sys, time, re, serial
ap = argparse.ArgumentParser()
ap.add_argument("port"); ap.add_argument("log"); ap.add_argument("server"); ap.add_argument("file")
ap.add_argument("--bootargs", default=None); ap.add_argument("--wait", type=float, default=900)
ap.add_argument("--after", type=float, default=90); ap.add_argument("--first", default=None)
ap.add_argument("--delay", type=float, default=0.02, help="seconds between characters; U-Boot drops fast input")
a = ap.parse_args()
log = open(a.log, "ab", buffering=0)
s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = 0.1; s.dtr = False; s.rts = False; s.open()
buf = b""
def note(msg):
    line = ("\n### %s %s\n" % (time.strftime("%H:%M:%S"), msg)).encode(); log.write(line); sys.stderr.write(line.decode())
def send(b):
    for ch in b:
        s.write(bytes([ch])); s.flush(); time.sleep(a.delay)
def pump():
    global buf
    d = s.read(65536)
    if d: log.write(d); buf += d
    return bool(d)
def wait_for(pats, timeout, spam=None, spam_every=0.3):
    global buf
    end = time.time() + timeout; last = 0
    while time.time() < end:
        pump()
        for p in pats:
            m = re.search(p, buf)
            if m:
                buf = buf[m.end():]; return p
        if spam and time.time() - last > spam_every:
            s.write(spam); last = time.time()
    return None
def cmd(line, prompt=rb"=> $", timeout=120):
    global buf
    buf = b""; note("cmd: " + line)
    send(line.encode() + b"\r")
    r = wait_for([prompt], timeout)
    if r is None: note("TIMEOUT waiting for prompt after: " + line); sys.exit(2)
if a.first:
    note("sending: " + a.first); send(a.first.encode() + b"\r")
note("waiting for U-Boot autoboot prompt")
if wait_for([rb"Hit any key to stop autoboot"], a.wait) is None:
    note("no U-Boot seen"); sys.exit(1)
if wait_for([rb"=> $"], 10, spam=b"\r") is None:
    note("no prompt after break-in"); sys.exit(1)
note("at U-Boot prompt")
cmd("setenv autoload no; dhcp", timeout=120)
cmd("tftpboot ${kernel_addr_r} %s:%s" % (a.server, a.file), timeout=180)
if a.bootargs:
    cmd("fdt addr ${fdtcontroladdr}", timeout=10)
    cmd("fdt set /chosen bootargs \"%s\"" % a.bootargs, timeout=10)
note("booting")
buf = b""; send(b"bootm ${kernel_addr_r} - ${fdtcontroladdr}\r")
end = time.time() + a.after
while time.time() < end: pump()
note("done listening")

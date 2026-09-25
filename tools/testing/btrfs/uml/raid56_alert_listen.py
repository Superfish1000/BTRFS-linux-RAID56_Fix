#!/usr/bin/env python3
# Listen for the ways btrfs tells userspace that RAID5/6 writes are in trouble,
# and write one line per event to a file.  Runs in the guest until killed.
#
#   raid56_alert_listen.py uevent  <out>
#   raid56_alert_listen.py poll    <out> <sysfs raid56_health file>
#   raid56_alert_listen.py fanotify <out> <mountpoint>
#
# uevent    KOBJ_CHANGE uevents that carry BTRFS_RAID56_*, from the kernel's
#           netlink socket (what udev rules see)
# poll      every wake-up of poll() on raid56_health, with the state it then reads
# fanotify  FAN_FS_ERROR events on the filesystem: the error and the file
import ctypes
import os
import select
import socket
import struct
import sys
import time

mode, out = sys.argv[1], sys.argv[2]
log = open(out, 'a', buffering=1)


def emit(line):
    log.write('%.1f %s\n' % (time.monotonic(), line))


if mode == 'uevent':
    NETLINK_KOBJECT_UEVENT = 15
    s = socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)
    s.bind((0, 1))
    emit('listening')
    while True:
        msg = s.recv(65536)
        fields = [f.decode(errors='replace') for f in msg.split(b'\0') if f]
        if any(f.startswith('BTRFS_RAID56') for f in fields):
            emit(' '.join(f for f in fields
                          if f.startswith(('ACTION=', 'DEVPATH=', 'BTRFS_RAID56'))))

elif mode == 'poll':
    path = sys.argv[3]
    f = open(path)
    state = f.read().split('\n')[0]
    emit('initial ' + state)
    p = select.poll()
    p.register(f, select.POLLPRI | select.POLLERR)
    while True:
        p.poll()
        f.seek(0)
        state = f.read().split('\n')[0]
        emit('woken ' + state)

elif mode == 'fanotify':
    mnt = sys.argv[3]
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fanotify_mark.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_uint64,
                                   ctypes.c_int, ctypes.c_char_p]
    FAN_CLASS_NOTIF, FAN_REPORT_FID = 0x0, 0x200
    FAN_MARK_ADD, FAN_MARK_FILESYSTEM = 0x1, 0x100
    FAN_FS_ERROR = 0x8000
    AT_FDCWD = -100
    fd = libc.fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID, os.O_RDONLY)
    if fd < 0:
        emit('fanotify_init failed errno %d' % ctypes.get_errno())
        sys.exit(1)
    if libc.fanotify_mark(fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, FAN_FS_ERROR,
                          AT_FDCWD, mnt.encode()) < 0:
        emit('fanotify_mark failed errno %d' % ctypes.get_errno())
        sys.exit(1)
    emit('listening')
    while True:
        buf = os.read(fd, 65536)
        off = 0
        while off + 24 <= len(buf):
            ev_len, _vers, _res, meta_len, mask, _efd, _pid = struct.unpack_from(
                '=IBBHQii', buf, off)
            error = count = None
            fid = ''
            i = off + meta_len
            while i + 4 <= off + ev_len:
                itype, _pad, ilen = struct.unpack_from('=BBH', buf, i)
                if itype == 5:          # FAN_EVENT_INFO_TYPE_ERROR
                    error, count = struct.unpack_from('=iI', buf, i + 4)
                elif itype == 1:        # FAN_EVENT_INFO_TYPE_FID
                    # fsid (8), then file_handle: handle_bytes, handle_type, f_handle
                    hb, = struct.unpack_from('=I', buf, i + 12)
                    fid = buf[i + 20:i + 20 + hb].hex()
                if ilen == 0:
                    break
                i += ilen
            emit('FAN_FS_ERROR mask 0x%x error %s count %s handle %s' %
                 (mask, error, count, fid or '-'))
            off += ev_len
else:
    sys.exit('unknown mode ' + mode)

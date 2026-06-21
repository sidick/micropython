# select.poll() over bsdsocket smoke test.
#
# extmod/modselect.c drives select.poll()/select.select() by calling each
# registered object's ioctl(MP_STREAM_POLL). ports/amiga/modsocket.c wires
# that up via a zero-timeout WaitSelect() on the socket fd. bsdsocket.library
# isn't available under vamos, so the live poll path is exercised on
# Amiberry; this test pins the module surface everywhere and the live
# readiness/NVAL behaviour wherever a TCP stack is present.

import select

# --- module surface ------------------------------------------------------

assert hasattr(select, "poll"), "poll"
assert hasattr(select, "select"), "select"

# poll() event-mask constants must match the upstream MP_STREAM_POLL_* values
# (POLLIN=1, POLLOUT=4) so user code is portable.
assert select.POLLIN == 1
assert select.POLLOUT == 4
assert select.POLLERR == 8
assert select.POLLHUP == 16

poller = select.poll()
for name in ("register", "unregister", "modify", "poll", "ipoll"):
    assert hasattr(poller, name), name

# --- live poll over a socket (Amiberry / real TCP stack) -----------------

# Under vamos socket() raises OSError and we stop here; on a real Amiga the
# rest exercises the WaitSelect-backed readiness reporting.
try:
    import socket

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
except OSError:
    print("OK")
    raise SystemExit

# A fresh, unbound UDP socket is writable but not readable.
poller.register(s)
events = poller.poll(0)
assert len(events) == 1, events
obj, flags = events[0]
assert obj is s, obj
assert flags & select.POLLOUT, flags
assert not (flags & select.POLLIN), flags

# register() is idempotent and modify() narrows the mask.
poller.register(s)
poller.modify(s, select.POLLIN)
poller.unregister(s)

# Polling a closed socket reports POLLNVAL (32) and does not block.
poller.register(s)
s.close()
events = poller.poll(0)
assert len(events) == 1, events
assert events[0][1] == 32, events  # POLLNVAL

print("OK")

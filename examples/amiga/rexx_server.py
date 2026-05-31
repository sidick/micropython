# Tiny ARexx server using amiga.rexx_serve.
#
# Opens a public ARexx port `EXAMPLE.<N>` (the suffix is auto-assigned
# so multiple instances can coexist) and dispatches incoming commands
# to a small handler. Drive it from another shell:
#
#     1> rx "address EXAMPLE.1 'PING'"
#     1> rx "address EXAMPLE.1 'TIME'"
#     1> rx "address EXAMPLE.1 'STOP'"

import time
import amiga

port_name = amiga.rexx_open("EXAMPLE")
print("listening on", port_name)
print("from another shell, try:")
print("  rx \"address %s 'PING'\"" % port_name)
print("  rx \"address %s 'TIME'\"" % port_name)
print("  rx \"address %s 'STOP'\"" % port_name)


def handle(command):
    # `command` is bytes; decode for matching.
    cmd = command.decode("latin-1").strip().upper()
    if cmd == "PING":
        return "pong"
    if cmd == "TIME":
        return "ticks=%d" % time.ticks_ms()
    if cmd == "STOP":
        # Returning StopIteration ends the serve loop cleanly.
        raise StopIteration
    raise ValueError("unknown command: " + cmd)


try:
    amiga.rexx_serve(handle, timeout_ms=10000)
finally:
    amiga.rexx_close()
    print("port closed")

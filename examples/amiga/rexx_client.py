# Outbound ARexx via amiga.rexx (one-shot) and amiga.RexxClient
# (persistent).
#
# Target host: pick something running on your system. `WORKBENCH` is
# present on every Workbench-2.0+ install and accepts a few commands.

import amiga

host = "WORKBENCH"

# One-shot send. Returns the host's result string (bytes).
try:
    result = amiga.rexx(host, "VERSION")
    print("WORKBENCH VERSION ->", result)
except OSError as e:
    print("WORKBENCH not responding:", e)
    raise SystemExit

# Use the persistent RexxClient when driving a host in a tight loop
# -- it holds an open reply MsgPort across sends so you don't pay the
# CreateMsgPort/DeleteMsgPort cost per call.
with amiga.RexxClient(host) as wb:
    print("INFO   ->", wb.send("INFO"))
    # check=False returns (rc, result_or_None) instead of raising.
    rc, result = wb.send("DEFINITELY NOT A COMMAND", check=False)
    print("bad cmd -> rc=%d result=%r" % (rc, result))

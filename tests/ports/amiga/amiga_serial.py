#!/usr/bin/env python3
"""Drive the Amiga port's REPL over its TCP serial console from the host.

The Amiberry "python.uae" config exposes the emulated Amiga's serial port
as a TCP socket on 127.0.0.1:1234 (override with --host/--port or the
AMIGA_SERIAL_HOST / AMIGA_SERIAL_PORT environment variables). For the
console to be live, the Amiga must attach a shell to the serial device on
boot — `newshell aux:` in S:user-startup does this. After that, connecting
to the socket lands at an ordinary AmigaDOS shell prompt; running
`micropython` from there enters the MicroPython REPL.

Only one client may use the serial console at a time: the Amiga side is a
single shell reading one byte stream, so a second concurrent reader (e.g. a
leftover REPL session alongside a fresh `newshell aux:`) splits the bytes
between them and input arrives scrambled. Close one before opening another.

Three things make the link quirky and are handled here:

  * Line ending. The Amiga side submits a line on CR ("\\r"), not LF. A
    bare LF does nothing useful. The target echoes "\\r\\n\\r" back.
  * Output is CRLF. Captured REPL output is normalised to LF so callers
    can compare against host-generated expected output.
  * No flow control. Bytes are still paced with a small per-byte delay
    (char_delay / AMIGA_SERIAL_DELAY) so the link can't overrun. The port
    now bulk-drains its serial RX (see mphalport.c amiga_rx_refill), which
    raised the reliable floor from ~5 ms/byte to ~1 ms; the 2 ms default
    leaves a comfortable margin (the overrun cliff is ~0.5 ms).

Importable API (for tests in this directory):

    from amiga_serial import AmigaSerial

    with AmigaSerial.session() as mp:        # waits for console, enters REPL
        out = mp.exec("import sys; print(sys.platform)")
        assert out.strip() == "amiga"

`AmigaSerial.session()` is the high-level entry point: it waits for the
serial console to come up (useful straight after a reboot), enters the
REPL, and yields a connected helper that exits the REPL and closes the
socket on the way out. The lower-level methods (connect, wait_for_console,
shell, enter_repl, exec) are available if a test needs finer control.

Command-line usage:

    tests/ports/amiga/amiga_serial.py                   # probe: print REPL banner
    tests/ports/amiga/amiga_serial.py -c "print(2 + 2)" # run one statement, print output
    tests/ports/amiga/amiga_serial.py -f script.py      # run a local .py file in the REPL
    tests/ports/amiga/amiga_serial.py --shell "version" # run one AmigaDOS shell command
    tests/ports/amiga/amiga_serial.py --wait            # block until the console is up
"""

import argparse
import contextlib
import os
import socket
import sys
import time

DEFAULT_HOST = os.environ.get("AMIGA_SERIAL_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("AMIGA_SERIAL_PORT", "1234"))

# The Amiga shell/REPL submits a line on CR; LF alone is ignored.
EOL = b"\r"

# Prompts we wait on. The AmigaDOS shell prompt is "<current-path>> "
# (preceded by a "\x0fN." console control sequence we ignore) — e.g.
# "Workbench:> " at a drive root or "py0:ports/amiga> " in a subdirectory.
# So the shell prompt always ends with ">", and is distinguished from the
# REPL's ">>> " only by *not* being a triple ">". The REPL also uses "... "
# for line continuation and "=== " in paste mode.
REPL_PROMPT = b">>> "
REPL_CONT = b"... "
PASTE_PROMPT = b"=== "

# Sentinels bracketing exec() output. They are emitted via chr(1) so the
# paste-mode *echo* of the source ("print(chr(1)+'...'+chr(1))") doesn't
# contain the raw \x01 bytes — only the executed output does. That lets us
# find the real output unambiguously even though paste mode echoes input.
_MARK = b"\x01"
_START = _MARK + b"AMIGASERIAL-START" + _MARK
_END = _MARK + b"AMIGASERIAL-END" + _MARK


def _write_all(fd, data):
    """os.write may write fewer bytes than requested on a pipe; loop."""
    while data:
        data = data[os.write(fd, data) :]


def _normalise(buf):
    """Decode Amiga console bytes to text with LF line endings. The Amiga
    terminates each line with "\\r\\n\\r" (CR LF CR), so collapse "\\r\\n"
    to "\\n" and drop any remaining lone CR rather than turning it into a
    second blank line."""
    return buf.replace(b"\r\n", b"\n").replace(b"\r", b"").decode(errors="replace")


class AmigaSerialError(Exception):
    pass


class AmigaSerial:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=5.0, char_delay=0.002):
        self.host = host
        self.port = port
        self.timeout = timeout
        # The Amiga serial line has no flow control, so bytes sent too fast
        # overrun and get dropped or transposed ("micropython" arriving as
        # "mcropython"). Pace transmission one byte at a time with a small
        # delay. Since the port started bulk-draining its serial RX the link
        # is reliable down to ~1 ms/byte (overrun cliff ~0.5 ms); 2 ms keeps
        # margin. Override via AMIGA_SERIAL_DELAY or the constructor.
        self.char_delay = float(os.environ.get("AMIGA_SERIAL_DELAY", char_delay))
        self.sock = None

    # -- connection ---------------------------------------------------

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        return self

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # -- low-level read/write ----------------------------------------

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        if self.char_delay <= 0:
            self.sock.sendall(data)
            return
        # Drip-feed one byte at a time so the flow-control-less Amiga
        # serial input doesn't overrun.
        for i in range(len(data)):
            self.sock.sendall(data[i : i + 1])
            time.sleep(self.char_delay)

    def _recv_until(self, pred, timeout=None):
        """Read until pred(buffer) is true or the socket goes idle for
        `timeout` seconds. Returns whatever was read."""
        deadline = None if timeout is None else time.monotonic() + timeout
        self.sock.settimeout(self.timeout if timeout is None else timeout)
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if pred(buf):
                break
            if deadline is not None and time.monotonic() > deadline:
                break
        return buf

    def drain(self, idle=1.0):
        """Read and discard whatever the target sends until it falls
        silent for `idle` seconds. Returns the drained bytes."""
        return self._recv_until(lambda b: False, timeout=idle)

    # -- console / shell ---------------------------------------------

    @staticmethod
    def _at_prompt(buf):
        """Classify the tail of `buf`: 'repl', 'shell', or None. A prior
        session may have left micropython running, so either prompt counts
        as a live console. The REPL's ">>> " must be checked before the
        generic shell ">" since it ends in ">" too."""
        s = buf.rstrip()
        if s.endswith(b">>>") or buf.endswith(REPL_CONT):
            return "repl"
        if s.endswith(b">"):
            return "shell"
        return None

    def sync(self, timeout=10.0):
        """Nudge with a CR and report which prompt we're at ('repl' or
        'shell'). Raises if neither appears."""
        self.send(EOL)
        buf = self._recv_until(lambda b: self._at_prompt(b) is not None, timeout=timeout)
        state = self._at_prompt(buf)
        if state is None:
            raise AmigaSerialError("no prompt; saw %r" % buf)
        return state

    def wait_for_console(self, timeout=150.0, poll=5.0):
        """Poll the socket until a prompt appears. Use after a reboot, when
        the listener may not exist yet and the OS is still coming up. Leaves
        a fresh connection open at whichever prompt is live; returns its
        kind ('repl' or 'shell')."""
        deadline = time.monotonic() + timeout
        last = b""
        while time.monotonic() < deadline:
            try:
                self.connect()
            except OSError:
                time.sleep(poll)
                continue
            self.send(EOL)
            last = self._recv_until(lambda b: self._at_prompt(b) is not None, timeout=poll)
            state = self._at_prompt(last)
            if state is not None:
                return state
            self.close()
            time.sleep(poll)
        raise AmigaSerialError(
            "serial console did not reach a prompt within %gs; last saw %r" % (timeout, last)
        )

    def to_shell(self, timeout=15.0):
        """Ensure we're at the AmigaDOS shell prompt, leaving the REPL
        first if a prior session is still in it."""
        if self.sync(timeout=timeout) == "repl":
            self.exit_repl(timeout=timeout)

    def shell(self, cmd, timeout=15.0):
        """Run one AmigaDOS shell command and return its output (with the
        command echo and trailing prompt stripped, CRLF normalised)."""
        self.to_shell(timeout=timeout)
        self.send(cmd.encode() + EOL)
        buf = self._recv_until(lambda b: self._at_prompt(b) == "shell", timeout=timeout)
        return self._strip(buf, cmd)

    # -- REPL --------------------------------------------------------

    def enter_repl(self, command=None, timeout=20.0):
        """Launch micropython from the shell and wait for the ">>> "
        prompt. Returns the REPL banner text. If a prior session already
        left us in the REPL, returns "" without relaunching.

        The launch command defaults to "micropython" but can be overridden
        with the AMIGA_SERIAL_MP env var, e.g.
        AMIGA_SERIAL_MP="micropython -X maxheap=8M" to cap the GC heap so
        out-of-memory tests can't starve AmigaOS of the RAM its serial I/O
        needs (which otherwise desyncs the console)."""
        if command is None:
            command = os.environ.get("AMIGA_SERIAL_MP", "micropython")
        if self.sync(timeout=timeout) == "repl":
            return ""
        self.send(command.encode() + EOL)
        buf = self._recv_until(lambda b: b.endswith(REPL_PROMPT), timeout=timeout)
        if not buf.endswith(REPL_PROMPT):
            raise AmigaSerialError("REPL did not start; saw %r" % buf)
        return self._strip(buf, command)

    def exit_repl(self, timeout=10.0):
        """Leave the REPL (Ctrl-D) and return to the shell prompt."""
        self.send(b"\x04")
        self._recv_until(lambda b: self._at_prompt(b) == "shell", timeout=timeout)

    def exec(self, code, timeout=20.0):
        """Run a snippet in the REPL via paste mode and return its stdout
        (CRLF normalised to LF). Paste mode avoids the auto-indent that
        would otherwise mangle multi-line code. If the snippet raises, the
        traceback text is returned in place of the (never-printed) tail."""
        self.send(b"\x05")  # Ctrl-E: enter paste mode
        self._recv_until(lambda b: b.endswith(PASTE_PROMPT), timeout=timeout)
        prog = (
            "print(chr(1)+'AMIGASERIAL-START'+chr(1))\n"
            + code
            + "\nprint(chr(1)+'AMIGASERIAL-END'+chr(1))"
        )
        self.send(prog.replace("\n", "\r").encode() + EOL)
        self.send(b"\x04")  # Ctrl-D: finish paste, execute
        buf = self._recv_until(lambda b: b.endswith(REPL_PROMPT), timeout=timeout)

        start = buf.find(_START)
        if start < 0:
            raise AmigaSerialError("exec produced no start marker; saw %r" % buf)
        body_start = start + len(_START)
        end = buf.find(_END, body_start)
        if end < 0:
            # Snippet raised before the end marker printed: return the tail
            # up to the prompt (contains the traceback).
            end = buf.rfind(REPL_PROMPT)
            if end < body_start:
                end = len(buf)
        out = buf[body_start:end]
        return _normalise(out).strip("\n")

    # -- pyboard bridge ----------------------------------------------

    def bridge(self):
        """Relay this socket to/from stdin/stdout as a raw serial line, so
        tools/pyboard.py (and thus run-tests.py -t "exec:...") can drive the
        Amiga's raw REPL. Assumes we're already at the ">>> " prompt.

        Two translations make the Amiga look like a standard serial board:

          * host->Amiga is drip-fed one byte at a time (char_delay) because
            the emulated serial line has no flow control and overruns on a
            burst.
          * Amiga->host has the AUX/Amiberry serial path's redundant CR
            removed. That path emits every newline as "\\r\\n\\r" (CR LF CR);
            dropping the CR that immediately follows each LF ("\\n\\r" -> "\\n")
            turns it back into a standard "\\r\\n" that pyboard and
            normalize_newlines understand. A 1-byte holdback (a trailing "\\n"
            whose following CR hasn't arrived yet) handles the sequence
            straddling recv() boundaries. Harmless no-op on an already-clean
            stream.
        """
        import select

        sock = self.sock
        stdin_fd = 0
        stdout_fd = 1
        pending = b""  # held-back tail that might begin a "\r\n\r"
        delay = self.char_delay
        while True:
            r, _, _ = select.select([stdin_fd, sock], [], [])
            if stdin_fd in r:
                data = os.read(stdin_fd, 4096)
                if not data:
                    break  # pyboard closed the pipe
                if delay > 0:
                    for i in range(len(data)):
                        sock.sendall(data[i : i + 1])
                        time.sleep(delay)
                else:
                    sock.sendall(data)
            if sock in r:
                chunk = sock.recv(4096)
                if not chunk:
                    break  # target closed
                data = (pending + chunk).replace(b"\n\r", b"\n")
                # Hold back a trailing "\n": the redundant CR that the Amiga
                # may emit right after it hasn't arrived yet, and we need the
                # next byte to know whether to drop it.
                if data.endswith(b"\n"):
                    data, pending = data[:-1], data[-1:]
                else:
                    pending = b""
                _write_all(stdout_fd, data)

    # -- helpers -----------------------------------------------------

    @staticmethod
    def _strip(buf, echoed_cmd):
        """Strip the leading command echo and the trailing prompt line from
        a response, and normalise CRLF to LF. The prompt is always the last
        line (it has no newline after it) and ends in ">", so dropping that
        line is prompt-agnostic — it works for both the shell's variable
        "<path>>" and the REPL's ">>> "."""
        text = _normalise(buf)
        lines = text.split("\n")
        if lines and lines[-1].rstrip().endswith(">"):
            lines = lines[:-1]
        if lines and echoed_cmd in lines[0]:
            lines = lines[1:]
        return "\n".join(lines).strip("\n")

    @classmethod
    @contextlib.contextmanager
    def session(cls, host=DEFAULT_HOST, port=DEFAULT_PORT, wait=True, wait_timeout=150.0):
        """High-level context manager: (optionally) wait for the console,
        enter the REPL, yield the helper, then exit the REPL and close."""
        self = cls(host, port)
        if wait:
            self.wait_for_console(timeout=wait_timeout)
        else:
            self.connect()
            self.sync()
        self.enter_repl()
        try:
            yield self
        finally:
            with contextlib.suppress(Exception):
                self.exit_repl()
            self.close()


# -- CLI ------------------------------------------------------------------


def main(argv=None):
    p = argparse.ArgumentParser(description="Drive the Amiga MicroPython REPL over TCP serial.")
    p.add_argument("-H", "--host", default=DEFAULT_HOST, help="host (default %(default)s)")
    p.add_argument(
        "-p", "--port", type=int, default=DEFAULT_PORT, help="port (default %(default)s)"
    )
    p.add_argument(
        "--wait", action="store_true", help="wait for the console to come up (post-reboot)"
    )
    p.add_argument(
        "--wait-timeout", type=float, default=150.0, help="seconds to wait for the console"
    )
    p.add_argument(
        "--delay",
        type=float,
        default=None,
        help="per-byte transmit delay in seconds (default 0.002 / AMIGA_SERIAL_DELAY)",
    )
    g = p.add_mutually_exclusive_group()
    g.add_argument("-c", "--command", help="run one statement in the REPL ('-' reads stdin)")
    g.add_argument("-f", "--file", help="run a local .py file in the REPL")
    g.add_argument("--shell", dest="shell_cmd", help="run one AmigaDOS shell command (no REPL)")
    g.add_argument(
        "--bridge",
        action="store_true",
        help="relay stdin/stdout to the raw REPL for pyboard.py / run-tests.py -t",
    )
    args = p.parse_args(argv)

    kwargs = {} if args.delay is None else {"char_delay": args.delay}
    mp = AmigaSerial(args.host, args.port, **kwargs)
    try:
        if args.wait:
            mp.wait_for_console(timeout=args.wait_timeout)
        else:
            mp.connect()
            mp.sync()
    except (OSError, AmigaSerialError) as e:
        print("amiga-serial: cannot reach console: %s" % e, file=sys.stderr)
        return 1

    try:
        if args.bridge:
            # Land on the REPL, then hand the raw byte stream to pyboard.
            # All diagnostics go to stderr so stdout stays a clean serial line.
            mp.enter_repl()
            mp.bridge()
            return 0
        if args.shell_cmd is not None:
            print(mp.shell(args.shell_cmd), end="")
            return 0

        banner = mp.enter_repl()
        if args.command is not None:
            code = sys.stdin.read() if args.command == "-" else args.command
            print(mp.exec(code), end="")
        elif args.file is not None:
            with open(args.file) as f:
                print(mp.exec(f.read()), end="")
        else:
            # Default probe: show the banner so a human can confirm the link.
            print(banner)
        mp.exit_repl()
        return 0
    except (OSError, AmigaSerialError) as e:
        print("amiga-serial: %s" % e, file=sys.stderr)
        return 1
    finally:
        mp.close()


if __name__ == "__main__":
    sys.exit(main())

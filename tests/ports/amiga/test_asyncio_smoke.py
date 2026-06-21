# asyncio core event-loop smoke test (no networking).
#
# Phase: asyncio enabled on the port (MICROPY_PY_ASYNCIO + scheduler, the
# asyncio package frozen from extmod/asyncio). The cooperative loop drives
# timers off time.ticks_* and idles in mp_event_wait (the
# MICROPY_INTERNAL_WFE slice sleep). This exercises tasks / sleep / gather /
# Event / Lock / wait_for -- everything but the select.poll IO path, which
# needs a live bsdsocket and is covered on Amiberry.

import asyncio
import time

# The asyncio loop is timer-driven (time.ticks_ms via timer.device ReadEClock).
# vamos doesn't emulate timer.device, so ticks are static and the loop would
# spin forever waiting for a sleep that never elapses. Detect that the same
# way test_time_monotonic_smoke does and soft-pass; the live run is on Amiberry.
_samples = [time.ticks_us() for _ in range(2000)]
if _samples[0] == _samples[-1]:
    print("OK")
    raise SystemExit

log = []


async def worker(name, ms):
    await asyncio.sleep_ms(ms)
    log.append(name)
    return name


async def main():
    # Ordering: timers fire in delay order regardless of spawn order.
    t1 = asyncio.create_task(worker("a", 30))
    t2 = asyncio.create_task(worker("b", 10))
    res = await asyncio.gather(t1, t2)
    assert res == ["a", "b"], res
    assert log == ["b", "a"], log

    # Event: a waiter unblocks when another task sets it.
    ev = asyncio.Event()

    async def setter():
        await asyncio.sleep_ms(5)
        ev.set()

    asyncio.create_task(setter())
    await ev.wait()
    assert ev.is_set()

    # Lock: mutual exclusion across tasks.
    lock = asyncio.Lock()
    order = []

    async def use_lock(tag):
        async with lock:
            order.append(tag + "1")
            await asyncio.sleep_ms(5)
            order.append(tag + "2")

    await asyncio.gather(asyncio.create_task(use_lock("x")), asyncio.create_task(use_lock("y")))
    # Critical sections must not interleave.
    assert order == ["x1", "x2", "y1", "y2"], order

    # wait_for returns the result when the coro finishes in time.
    assert await asyncio.wait_for(worker("c", 5), 1.0) == "c"

    # wait_for raises TimeoutError when it doesn't. (Wrap the sleep in a
    # coroutine: MicroPython's sleep_ms() returns a SingletonGenerator, not a
    # coroutine create_task() accepts.)
    async def slow():
        await asyncio.sleep_ms(100)

    try:
        await asyncio.wait_for(slow(), 0.01)
        raise AssertionError("no timeout")
    except asyncio.TimeoutError:
        pass


asyncio.run(main())
print("OK")

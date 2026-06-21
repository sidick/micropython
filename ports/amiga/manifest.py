freeze("$(PORT_DIR)/modules")

# asyncio package (event loop + streams). Pulled from extmod/asyncio so
# `import asyncio` works against the cooperative event loop.
include("$(MPY_DIR)/extmod/asyncio")

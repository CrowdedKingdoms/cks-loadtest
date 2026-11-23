#!/usr/bin/env python3
"""Add-only climb for the four-server mesh (west, east, north, ne).

Does not clear bots, does not kill a running JVM, and does not start unless invoked.
"""
from climb_mesh import four_mesh, run

if __name__ == "__main__":
    run(four_mesh())

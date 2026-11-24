#!/usr/bin/env python3
"""Add-only climb for the nine-server mesh.

Does not clear the four-server or sixteen-server bots. Does not start unless invoked.
"""
from climb_mesh import nine_mesh, run

if __name__ == "__main__":
    run(nine_mesh())

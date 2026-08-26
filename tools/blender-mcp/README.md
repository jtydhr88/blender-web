# Blender MCP integration

`addon.py` is vendored from [ahujasid/blender-mcp](https://github.com/ahujasid/blender-mcp)
(MIT license). It is copied into the `-mcp` release zip as
`blender_mcp_addon.py` together with `blender-with-mcp.bat`, so AI assistants
can control the streamed Blender through MCP while the browser shows the
result live. The plain zip ships without these files.

Update it by replacing `addon.py` with the upstream version:

    Invoke-WebRequest https://raw.githubusercontent.com/ahujasid/blender-mcp/main/addon.py -OutFile addon.py

The addon auto-starts its socket server on `localhost:9876` when loaded via
`--python` (register() has an auto-start path). The socket executes arbitrary
Python inside Blender — same local-only trust model as the web port (7681):
fine on a private machine, do not expose either to untrusted networks.

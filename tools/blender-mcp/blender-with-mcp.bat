@echo off
rem Launch blender-web with the Blender MCP addon loaded. The addon starts a
rem localhost-only socket server (port 9876) that MCP clients connect to via
rem `uvx blender-mcp`.
"%~dp0blender.exe" --python "%~dp0blender_mcp_addon.py" %*

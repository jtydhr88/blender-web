@echo off
rem Launch blender-web for ComfyTV: MCP addon (port 9876) + ComfyTV bridge
rem (JSON job API on port 7684, override with BLENDER_COMFYTV_PORT).
rem ComfyTV probes and connects the same way it does Eagle: the user runs
rem this, ComfyTV finds it.
"%~dp0blender.exe" --python "%~dp0blender_mcp_addon.py" --python "%~dp0blender_comfytv_bridge.py" %*

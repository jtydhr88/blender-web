from .nodes import NODE_CLASS_MAPPINGS, NODE_DISPLAY_NAME_MAPPINGS
from server import PromptServer
from aiohttp import web, ClientSession

WEB_DIRECTORY = "./js"

routes = PromptServer.instance.routes


@routes.get("/blender-web/capture")
async def blender_web_capture(request):
    """Proxy /capture from Blender to avoid CORS."""
    server_url = request.query.get("server", "http://localhost:7681")
    capture_url = server_url.rstrip("/") + "/capture"
    try:
        async with ClientSession() as session:
            async with session.get(capture_url, timeout=5) as resp:
                if resp.status != 200:
                    return web.Response(status=resp.status, text="Blender capture failed")
                data = await resp.read()
                return web.Response(
                    body=data,
                    content_type="image/jpeg",
                    headers={"Cache-Control": "no-cache"},
                )
    except Exception as e:
        return web.Response(status=503, text=f"Cannot reach Blender: {e}")


@routes.get("/blender-web/cameras")
async def blender_web_cameras(request):
    """Proxy camera list from Blender."""
    server_url = request.query.get("server", "http://localhost:7681")
    from urllib.parse import urlparse
    parsed = urlparse(server_url)
    host = parsed.hostname or "localhost"
    port = (parsed.port or 7681) + 1
    api_url = f"http://{host}:{port}/cameras"
    try:
        async with ClientSession() as session:
            async with session.get(api_url, timeout=5) as resp:
                data = await resp.read()
                return web.Response(body=data, content_type="application/json")
    except Exception as e:
        return web.json_response({"cameras": [], "error": str(e)})


__all__ = ["NODE_CLASS_MAPPINGS", "NODE_DISPLAY_NAME_MAPPINGS", "WEB_DIRECTORY"]

# SPDX-License-Identifier: GPL-2.0-or-later
"""ComfyTV bridge: a JSON job API served from inside blender-web.

Design follows Blender's own architecture (scene -> render layers ->
compositor) extended across the ComfyTV boundary: the SCENE is the single
source of truth. ComfyTV nodes are datablock references, not function calls —
render settings, frame ranges and camera parameters live in the scene and are
edited in the (embedded) Blender UI. The bridge therefore:

  - imports models ADDITIVELY into a "ComfyTV" collection (an asset drop,
    never a scene clear),
  - lists camera datablocks plus the scene truth they render with,
  - creates rigs as real, user-editable scene objects (a convenience action,
    not a stage parameter),
  - renders with whatever the scene says (a render request only picks the
    camera and still/animation mode — F12 semantics).

Load via the bundled launcher:

    blender-for-comfytv.bat        (blender.exe --python blender_comfytv_bridge.py)

Endpoints (default port 7684, override with BLENDER_COMFYTV_PORT):

    GET  /comfytv/status            identity + scene summary (probe target)
    GET  /comfytv/cameras           camera datablocks + scene render truth
    POST /comfytv/import            {"path": ...} -> into the ComfyTV collection
    POST /comfytv/render            {"camera": ..., "mode": "still"|"animation",
                                     "shading": "clay"|"full"}
    GET  /comfytv/jobs/<id>         poll job status/progress/result

HTTP runs on a worker thread; everything that touches bpy is queued to the
main thread through a bpy.app.timers pump. Renders use INVOKE_DEFAULT so the
UI (and the web stream) stays live; progress comes from render handlers.
"""
import json
import os
import threading
import traceback
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import bpy

PORT = int(os.environ.get("BLENDER_COMFYTV_PORT", "7684"))
BRIDGE_VERSION = "0.1.0"
COLLECTION_NAME = "ComfyTV"
MODEL_EXTENSIONS = ('.glb', '.gltf', '.obj', '.fbx', '.usd', '.usdc', '.usdz')

# ComfyTV announces its own base URL in the X-ComfyTV-Base header of every
# probe; the asset panel calls back through it. Written by the HTTP thread,
# read by the UI — a plain dict slot is enough for a string swap.
_comfytv_base = {"url": ""}
_asset_cache: list[dict] = []

_jobs: dict[str, dict] = {}
_jobs_lock = threading.Lock()
_main_queue: list[tuple[str, dict]] = []
_queue_lock = threading.Lock()
_sync_done = threading.Event()
_sync_result: dict = {}


# ------------------------------------------------------------------ helpers

def _scene_truth() -> dict:
    """The render-relevant scene state ComfyTV mirrors read-only."""
    scene = bpy.context.scene
    return {
        "resolution_x": scene.render.resolution_x,
        "resolution_y": scene.render.resolution_y,
        "frame_start": scene.frame_start,
        "frame_end": scene.frame_end,
        "fps": scene.render.fps,
        "engine": scene.render.engine,
        "samples": scene.eevee.taa_render_samples if hasattr(scene, 'eevee') else None,
        "active_camera": scene.camera.name if scene.camera else None,
    }


def _scene_summary() -> dict:
    return {
        "objects": len(bpy.data.objects),
        "meshes": sum(1 for o in bpy.data.objects if o.type == 'MESH'),
        **_scene_truth(),
    }


def _list_cameras(_params: dict) -> dict:
    scene = bpy.context.scene
    cameras = []
    for obj in bpy.data.objects:
        if obj.type != 'CAMERA':
            continue
        cameras.append({
            "name": obj.name,
            "active": obj is scene.camera,
            "lens": obj.data.lens,
        })
    return {"cameras": cameras, "scene": _scene_truth()}


def _object_radius(obj) -> float:
    from mathutils import Vector
    corners = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    lo = Vector(map(min, *corners))
    hi = Vector(map(max, *corners))
    return (hi - lo).length / 2


def _mesh_bounds() -> tuple:
    """World-space center and radius of the mesh objects that matter.

    Ground planes and backdrops dwarf the subject and would push the camera
    far out, so objects much larger than the median are left out of framing.
    """
    from mathutils import Vector
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    if not meshes:
        return Vector((0, 0, 0)), 1.0

    radii = sorted(_object_radius(o) for o in meshes)
    median = radii[len(radii) // 2]
    subject = [o for o in meshes if _object_radius(o) <= median * 4] or meshes

    lo = Vector((1e30,) * 3)
    hi = Vector((-1e30,) * 3)
    for obj in subject:
        for corner in obj.bound_box:
            world = obj.matrix_world @ Vector(corner)
            lo = Vector(map(min, lo, world))
            hi = Vector(map(max, hi, world))
    center = (lo + hi) / 2
    radius = max((hi - lo).length / 2, 0.01)
    return center, radius


def _comfytv_collection():
    coll = bpy.data.collections.get(COLLECTION_NAME)
    if coll is None:
        coll = bpy.data.collections.new(COLLECTION_NAME)
        bpy.context.scene.collection.children.link(coll)
    elif COLLECTION_NAME not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(coll)
    return coll


def _do_import(params: dict) -> dict:
    """Asset drop: add a model into the ComfyTV collection. Never clears."""
    path = params.get("path", "")
    if not path or not os.path.isfile(path):
        raise ValueError(f"file not found: {path!r}")

    before = set(bpy.data.objects)
    ext = os.path.splitext(path)[1].lower()
    if ext in ('.glb', '.gltf'):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == '.obj':
        bpy.ops.wm.obj_import(filepath=path)
    elif ext == '.fbx':
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext in ('.usd', '.usdc', '.usdz'):
        bpy.ops.wm.usd_import(filepath=path)
    else:
        raise ValueError(f"unsupported model format: {ext}")

    coll = _comfytv_collection()
    imported = [o for o in bpy.data.objects if o not in before]
    for obj in imported:
        for other in list(obj.users_collection):
            other.objects.unlink(obj)
        coll.objects.link(obj)

    center, radius = _mesh_bounds()
    return {"imported": [o.name for o in imported],
            "collection": COLLECTION_NAME,
            "center": list(center), "radius": radius}


def _job_update(job_id: str, **fields):
    with _jobs_lock:
        if job_id in _jobs:
            _jobs[job_id].update(fields)


def _apply_clay(scene):
    """Switch to a Workbench clay setup for driving-video renders.

    ComfyTV's AI stages take silhouette, motion and camera from a Blender
    render — style comes from their reference image. Workbench compiles no
    material shaders and loads no textures, so clay renders stay light enough
    to share the GPU with ComfyUI. Returns a restore callback.
    """
    shading = scene.display.shading
    saved = {
        "engine": scene.render.engine,
        "use_nodes": scene.use_nodes,
        "single_layer": scene.render.use_single_layer,
        "film_transparent": scene.render.film_transparent,
        "light": shading.light,
        "color_type": shading.color_type,
        "single_color": tuple(shading.single_color),
        "show_cavity": shading.show_cavity,
        "show_shadows": shading.show_shadows,
    }
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.use_nodes = False
    scene.render.use_single_layer = True
    scene.render.film_transparent = False
    shading.light = 'STUDIO'
    shading.color_type = 'SINGLE'
    shading.single_color = (0.8, 0.8, 0.8)
    shading.show_cavity = True
    shading.show_shadows = True

    def restore():
        scene.render.engine = saved["engine"]
        scene.use_nodes = saved["use_nodes"]
        scene.render.use_single_layer = saved["single_layer"]
        scene.render.film_transparent = saved["film_transparent"]
        shading.light = saved["light"]
        shading.color_type = saved["color_type"]
        shading.single_color = saved["single_color"]
        shading.show_cavity = saved["show_cavity"]
        shading.show_shadows = saved["show_shadows"]

    return restore


def _start_render_job(job_id: str, params: dict):
    """F12 semantics: render with the scene's own settings.

    The request only picks the camera (a datablock reference) and the mode;
    resolution, samples, engine and frame range all come from the scene.
    """
    scene = bpy.context.scene
    mode = params.get("mode", "still")
    shading = params.get("shading", "clay")

    cam_name = params.get("camera") or ""
    if cam_name:
        cam = bpy.data.objects.get(cam_name)
        if cam is None or cam.type != 'CAMERA':
            raise ValueError(f"no camera named {cam_name!r} in the scene")
        scene.camera = cam
    if scene.camera is None:
        raise RuntimeError("the scene has no active camera")

    bpy.context.preferences.view.render_display_type = 'NONE'

    out_dir = os.path.join(bpy.app.tempdir, "comfytv_bridge")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, f"render_{job_id[:8]}_")

    restore = _apply_clay(scene) if shading == "clay" else (lambda: None)

    if mode == "still":
        try:
            scene.render.image_settings.media_type = 'IMAGE'
            scene.render.image_settings.file_format = 'PNG'
            scene.render.filepath = stem + "still.png"
            bpy.ops.render.render(write_still=True)
        finally:
            restore()
        _job_update(job_id, status="done", progress=1.0,
                    result={"path": scene.render.filepath, "frames": 1})
        return

    total = scene.frame_end - scene.frame_start + 1
    scene.render.image_settings.media_type = 'VIDEO'
    scene.render.image_settings.file_format = 'FFMPEG'
    scene.render.ffmpeg.format = 'MPEG4'
    scene.render.ffmpeg.codec = 'H264'
    scene.render.ffmpeg.constant_rate_factor = 'HIGH'
    scene.render.filepath = stem

    state = {"written": 0}

    def _on_write(scene_, _depsgraph=None):
        state["written"] += 1
        _job_update(job_id, progress=state["written"] / total)

    def _find_output() -> str:
        candidates = [f for f in os.listdir(out_dir)
                      if f.startswith(os.path.basename(stem)) and f.endswith('.mp4')]
        if not candidates:
            raise RuntimeError("render finished but no mp4 found")
        return os.path.join(out_dir, sorted(candidates)[-1])

    def _on_complete(scene_, _depsgraph=None):
        _cleanup()
        try:
            path = _find_output()
            _job_update(job_id, status="done", progress=1.0,
                        result={"path": path, "frames": total,
                                "size_bytes": os.path.getsize(path)})
        except Exception as exc:
            _job_update(job_id, status="error", error=str(exc))

    def _on_cancel(scene_, _depsgraph=None):
        _cleanup()
        _job_update(job_id, status="error", error="render cancelled")

    def _cleanup():
        for handler_list, fn in ((bpy.app.handlers.render_write, _on_write),
                                 (bpy.app.handlers.render_complete, _on_complete),
                                 (bpy.app.handlers.render_cancel, _on_cancel)):
            if fn in handler_list:
                handler_list.remove(fn)
        restore()

    bpy.app.handlers.render_write.append(_on_write)
    bpy.app.handlers.render_complete.append(_on_complete)
    bpy.app.handlers.render_cancel.append(_on_cancel)

    result = bpy.ops.render.render('INVOKE_DEFAULT', animation=True)
    if 'CANCELLED' in result:
        _cleanup()
        raise RuntimeError("render could not start (another render running?)")


# --------------------------------------------------- main-thread dispatcher

_SYNC_ACTIONS = {
    "import": _do_import,
    "cameras": _list_cameras,
}


def _pump():
    with _queue_lock:
        items, _main_queue[:] = _main_queue[:], []
    for kind, payload in items:
        try:
            if kind == "sync":
                action = _SYNC_ACTIONS[payload["action"]]
                _sync_result.clear()
                _sync_result["ok"] = action(payload["params"])
            elif kind == "render":
                job_id = payload["job_id"]
                _job_update(job_id, status="running")
                _start_render_job(job_id, payload["params"])
        except Exception as exc:
            traceback.print_exc()
            if kind == "sync":
                _sync_result.clear()
                _sync_result["error"] = str(exc)
            else:
                _job_update(payload["job_id"], status="error", error=str(exc))
        finally:
            if kind == "sync":
                _sync_done.set()
    return 0.1


def _run_sync(action: str, params: dict, timeout: float = 120.0) -> dict:
    """Run an action on the main thread and wait for its result."""
    _sync_done.clear()
    with _queue_lock:
        _main_queue.append(("sync", {"action": action, "params": params}))
    if not _sync_done.wait(timeout):
        raise TimeoutError(f"{action} timed out after {timeout}s")
    if "error" in _sync_result:
        raise RuntimeError(_sync_result["error"])
    return _sync_result["ok"]


# ------------------------------------------------------- asset panel (N key)

def _fetch_comfytv_assets() -> list[dict]:
    """Pull Blender-importable model assets from the ComfyTV library."""
    import urllib.parse
    import urllib.request
    base = _comfytv_base["url"]
    if not base:
        raise RuntimeError("ComfyTV has not connected yet")
    with urllib.request.urlopen(f"{base}/comfytv/assets?limit=500", timeout=10) as resp:
        data = json.loads(resp.read())
    models = []
    for asset in data.get("assets", []):
        if asset.get("media_type") != "model":
            continue
        payload_url = str(asset.get("payload_url") or "")
        query = urllib.parse.parse_qs(urllib.parse.urlparse(payload_url).query)
        filename = (query.get("filename") or [""])[0]
        if not filename.lower().endswith(MODEL_EXTENSIONS):
            continue
        models.append({
            "name": asset.get("name") or filename or f"#{asset.get('id')}",
            "payload_url": payload_url,
            "filename": filename,
        })
    return models


def _download_asset(payload_url: str, filename: str) -> str:
    import urllib.request
    base = _comfytv_base["url"]
    out_dir = os.path.join(bpy.app.tempdir, "comfytv_assets")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, os.path.basename(filename))
    with urllib.request.urlopen(f"{base}{payload_url}", timeout=60) as resp, \
            open(path, "wb") as out:
        out.write(resp.read())
    return path


class COMFYTV_OT_refresh_assets(bpy.types.Operator):
    bl_idname = "comfytv.refresh_assets"
    bl_label = "ComfyTV: Refresh Assets"
    bl_description = "Reload the model list from the ComfyTV asset library"

    def execute(self, context):
        try:
            _asset_cache[:] = _fetch_comfytv_assets()
        except Exception as exc:
            self.report({'ERROR'}, f"ComfyTV assets: {exc}")
            return {'CANCELLED'}
        self.report({'INFO'}, f"{len(_asset_cache)} model asset(s)")
        return {'FINISHED'}


class COMFYTV_OT_import_asset(bpy.types.Operator):
    bl_idname = "comfytv.import_asset"
    bl_label = "ComfyTV: Import Asset"
    bl_description = "Import this asset into the ComfyTV collection"

    payload_url: bpy.props.StringProperty()
    filename: bpy.props.StringProperty()

    def execute(self, context):
        try:
            path = _download_asset(self.payload_url, self.filename)
            result = _do_import({"path": path})
        except Exception as exc:
            self.report({'ERROR'}, f"import failed: {exc}")
            return {'CANCELLED'}
        self.report({'INFO'}, f"imported {', '.join(result['imported'])}")
        return {'FINISHED'}


class COMFYTV_PT_assets(bpy.types.Panel):
    bl_idname = "COMFYTV_PT_assets"
    bl_label = "ComfyTV Assets"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "ComfyTV"

    def draw(self, context):
        layout = self.layout
        if not _comfytv_base["url"]:
            layout.label(text="Waiting for ComfyTV…", icon='INFO')
            return
        layout.operator("comfytv.refresh_assets", text="Refresh", icon='FILE_REFRESH')
        if not _asset_cache:
            layout.label(text="No model assets (refresh?)")
            return
        col = layout.column(align=True)
        for asset in _asset_cache:
            row = col.row(align=True)
            row.label(text=asset["name"], icon='MESH_MONKEY')
            props = row.operator("comfytv.import_asset", text="", icon='IMPORT')
            props.payload_url = asset["payload_url"]
            props.filename = asset["filename"]


_UI_CLASSES = (COMFYTV_OT_refresh_assets, COMFYTV_OT_import_asset, COMFYTV_PT_assets)


# ------------------------------------------------------------------ server

class _Handler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):  # quiet
        pass

    def _reply(self, code: int, payload: dict):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        return json.loads(self.rfile.read(length))

    def do_GET(self):
        try:
            base = self.headers.get("X-ComfyTV-Base")
            if base:
                _comfytv_base["url"] = base.rstrip("/")
            if self.path == "/comfytv/status":
                self._reply(200, {
                    "app": "blender-web",
                    "bridge_version": BRIDGE_VERSION,
                    "blender_version": bpy.app.version_string,
                    "web_port": int(os.environ.get("BLENDER_WEB_PORT", "7681")),
                    "scene": _scene_summary(),
                })
            elif self.path == "/comfytv/cameras":
                self._reply(200, _run_sync("cameras", {}))
            elif self.path.startswith("/comfytv/jobs/"):
                job_id = self.path.rsplit("/", 1)[-1]
                with _jobs_lock:
                    job = dict(_jobs.get(job_id) or {})
                if not job:
                    self._reply(404, {"error": "unknown job"})
                else:
                    self._reply(200, job)
            else:
                self._reply(404, {"error": "unknown endpoint"})
        except Exception as exc:
            self._reply(500, {"error": str(exc)})

    def do_POST(self):
        try:
            params = self._body()
            if self.path == "/comfytv/import":
                self._reply(200, _run_sync("import", params))
            elif self.path == "/comfytv/render":
                job_id = uuid.uuid4().hex
                with _jobs_lock:
                    _jobs[job_id] = {"id": job_id, "status": "queued", "progress": 0.0}
                with _queue_lock:
                    _main_queue.append(("render", {"job_id": job_id, "params": params}))
                self._reply(200, {"job_id": job_id})
            else:
                self._reply(404, {"error": "unknown endpoint"})
        except Exception as exc:
            self._reply(500, {"error": str(exc)})


def _serve():
    server = ThreadingHTTPServer(("127.0.0.1", PORT), _Handler)
    print(f"[comfytv-bridge] listening on http://127.0.0.1:{PORT}")
    server.serve_forever()


def register():
    for cls in _UI_CLASSES:
        bpy.utils.register_class(cls)
    threading.Thread(target=_serve, daemon=True, name="comfytv-bridge").start()
    bpy.app.timers.register(_pump, persistent=True)
    print(f"[comfytv-bridge] v{BRIDGE_VERSION} registered")


if __name__ == "__main__":
    register()

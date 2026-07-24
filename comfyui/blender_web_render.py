"""
Blender Web Render API — automatically loaded on startup.

Provides an HTTP render endpoint so external tools (ComfyUI) can trigger
a full Cycles/EEVEE render and retrieve the result image.
"""

import bpy
import os
import sys
import json
import tempfile
import threading
import traceback
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

_render_request = None
_render_result = None
_render_error = None
_render_lock = threading.Lock()
_render_event = threading.Event()

RENDER_PORT = int(os.environ.get("BLENDER_WEB_PORT", "7681")) + 1
RENDER_BIND = os.environ.get("BLENDER_WEB_BIND", "127.0.0.1")

# Camera preview cache: only re-render when scene changes.
_preview_cache = None       # bytes (BMP data)
_preview_dirty = True       # True = need re-render
_preview_cam_name = None    # Track which camera was rendered
_preview_size = (0, 0)      # Track render size


def _enqueue_cmd(cmd):
    """Queue a command for the main thread and reset the event."""
    global _render_request, _render_result
    with _render_lock:
        _render_result = None
        _render_request = cmd
    _render_event.clear()


def _cmd_take_result():
    """Take the result (must hold _render_lock or call under lock)."""
    global _render_result
    r = _render_result
    _render_result = None
    return r


def _log(msg):
    sys.stderr.write(f"[BlenderWebRender] {msg}\n")
    sys.stderr.flush()


class RenderHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(200)
        self._cors()
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)

        if path == "/render":
            self._handle_render(params)
        elif path == "/preview":
            self._handle_preview(params)
        elif path == "/cameras":
            self._handle_cameras()
        elif path == "/camera/active":
            self._handle_set_active_camera(params)
        elif path == "/camera/view":
            self._handle_camera_view(params)
        elif path == "/camera/params":
            self._handle_camera_params(params)
        elif path == "/scene":
            self._handle_scene()
        elif path == "/render/animation":
            self._handle_render_animation(params)
        elif path == "/render/video":
            self._handle_render_video(params)
        elif path == "/status":
            self._send_json(200, {"status": "ok", "port": RENDER_PORT})
        else:
            self._send_json(404, {"error": "not found"})

    def _handle_render(self, params):
        global _render_request, _render_result, _render_error

        width = int(params.get("width", [1024])[0])
        height = int(params.get("height", [1024])[0])
        samples = int(params.get("samples", [32])[0])
        engine = params.get("engine", ["BLENDER_EEVEE"])[0].upper()

        if engine not in ("CYCLES", "BLENDER_EEVEE", "OPENGL"):
            engine = "BLENDER_EEVEE"

        _log(f"Render request: {width}x{height} {engine} samples={samples}")

        with _render_lock:
            _render_result = None
            _render_error = None
            _render_request = {
                "width": width,
                "height": height,
                "samples": samples,
                "engine": engine,
            }

        _render_event.clear()

        if not _render_event.wait(timeout=120):
            self._send_json(504, {"error": "render timeout"})
            return

        with _render_lock:
            result = _render_result
            error = _render_error
            _render_result = None
            _render_error = None

        if error:
            self._send_json(500, {"error": error})
            return

        if result and os.path.exists(result):
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self._cors()
            data = open(result, "rb").read()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            try:
                os.unlink(result)
            except OSError:
                pass
            _log(f"Render complete: {len(data)} bytes")
        else:
            self._send_json(500, {"error": "render produced no output"})

    def _wait_result(self, timeout=10):
        """Wait for main thread to process command and return result."""
        if not _render_event.wait(timeout=timeout):
            return None, True
        with _render_lock:
            return _cmd_take_result(), False

    def _handle_preview(self, params):
        """Quick OpenGL preview from the active camera — fast enough for polling."""
        width = int(params.get("width", [640])[0])
        height = int(params.get("height", [480])[0])
        _enqueue_cmd({"action": "opengl_preview", "width": width, "height": height})
        result, timeout = self._wait_result()
        if timeout:
            self._send_json(504, {"error": "timeout"})
            return
        if isinstance(result, dict) and "_bytes" in result:
            # In-memory image data.
            self.send_response(200)
            self.send_header("Content-Type", result["_content_type"])
            self._cors()
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Content-Length", str(len(result["_bytes"])))
            self.end_headers()
            self.wfile.write(result["_bytes"])
        elif isinstance(result, dict) and "error" in result:
            self._send_json(500, result)
        else:
            self._send_json(500, {"error": "preview failed"})

    def _handle_scene(self):
        """Return scene animation/render info."""
        _enqueue_cmd({"action": "scene_info"})
        result, timeout = self._wait_result()
        if timeout: self._send_json(504, {"error": "timeout"}); return
        self._send_json(200, result or {})

    def _handle_render_animation(self, params):
        """Render an animation frame range and return all frames as a JSON array of base64 PNGs."""
        width = int(params.get("width", [1024])[0])
        height = int(params.get("height", [1024])[0])
        samples = int(params.get("samples", [32])[0])
        engine = params.get("engine", ["BLENDER_EEVEE"])[0].upper()
        start = int(params.get("start", [1])[0])
        end = int(params.get("end", [10])[0])
        step = int(params.get("step", [1])[0])
        camera = params.get("camera", [""])[0]

        if engine not in ("CYCLES", "BLENDER_EEVEE"):
            engine = "BLENDER_EEVEE"
        if step < 1:
            step = 1

        _enqueue_cmd({
            "action": "render_animation",
            "width": width, "height": height,
            "samples": samples, "engine": engine,
            "start": start, "end": end, "step": step,
            "camera": camera,
        })

        # Animation can take a long time (10min max).
        result, timeout = self._wait_result(timeout=600)
        if timeout: self._send_json(504, {"error": "timeout"}); return

        if isinstance(result, dict) and "_frames" in result:
            # Return frame file paths as JSON.
            self._send_json(200, {"frames": result["_frames"], "count": len(result["_frames"])})
        elif isinstance(result, dict) and "error" in result:
            self._send_json(500, result)
        else:
            self._send_json(500, {"error": "animation render failed"})

    def _handle_render_video(self, params):
        """Render animation directly to video file using Blender's native FFmpeg pipeline."""
        width = int(params.get("width", [1920])[0])
        height = int(params.get("height", [1080])[0])
        samples = int(params.get("samples", [32])[0])
        engine = params.get("engine", ["BLENDER_EEVEE"])[0].upper()
        start = int(params.get("start", [1])[0])
        end = int(params.get("end", [60])[0])
        step = int(params.get("step", [1])[0])
        camera = params.get("camera", [""])[0]
        fps = int(params.get("fps", [24])[0])
        # FFmpeg settings.
        container = params.get("container", ["MPEG4"])[0].upper()
        codec = params.get("codec", ["H264"])[0].upper()
        quality = params.get("quality", ["MEDIUM"])[0].upper()
        video_bitrate = int(params.get("video_bitrate", [6000])[0])
        audio_codec = params.get("audio_codec", ["AAC"])[0].upper()
        audio_bitrate = int(params.get("audio_bitrate", [192])[0])
        output_path = params.get("output_path", [""])[0]
        film_transparent = params.get("film_transparent", ["0"])[0] == "1"

        if engine not in ("CYCLES", "BLENDER_EEVEE", "OPENGL"):
            engine = "BLENDER_EEVEE"
        if step < 1:
            step = 1

        _enqueue_cmd({
            "action": "render_video",
            "width": width, "height": height,
            "samples": samples, "engine": engine,
            "start": start, "end": end, "step": step,
            "camera": camera, "fps": fps,
            "container": container, "codec": codec,
            "quality": quality, "video_bitrate": video_bitrate,
            "audio_codec": audio_codec, "audio_bitrate": audio_bitrate,
            "output_path": output_path,
            "film_transparent": film_transparent,
        })

        # Video render can take a very long time.
        result, timeout = self._wait_result(timeout=3600)
        if timeout: self._send_json(504, {"error": "timeout"}); return

        if isinstance(result, dict) and "video_path" in result:
            self._send_json(200, result)
        elif isinstance(result, dict) and "error" in result:
            self._send_json(500, result)
        else:
            self._send_json(500, {"error": "video render failed"})

    def _handle_cameras(self):
        _enqueue_cmd({"action": "list_cameras"})
        result, timeout = self._wait_result()
        if timeout: self._send_json(504, {"error": "timeout"}); return
        self._send_json(200, result or {"cameras": []})

    def _handle_set_active_camera(self, params):
        name = params.get("name", [""])[0]
        if not name: self._send_json(400, {"error": "name required"}); return
        _enqueue_cmd({"action": "set_active_camera", "name": name})
        result, timeout = self._wait_result()
        if timeout: self._send_json(504, {"error": "timeout"}); return
        self._send_json(200, result or {"ok": True})

    def _handle_camera_view(self, params):
        enable = params.get("enable", ["1"])[0] != "0"
        _enqueue_cmd({"action": "camera_view", "enable": enable})
        result, timeout = self._wait_result()
        if timeout: self._send_json(504, {"error": "timeout"}); return
        self._send_json(200, result or {"ok": True})

    def _handle_camera_params(self, params):
        name = params.get("name", [""])[0]
        cmd = {"action": "camera_params", "name": name}
        for key in ("focal_length", "sensor_width", "clip_start", "clip_end",
                     "loc_x", "loc_y", "loc_z", "rot_x", "rot_y", "rot_z"):
            if key in params:
                cmd[key] = float(params[key][0])
        _enqueue_cmd(cmd)
        result, timeout = self._wait_result()
        if timeout: self._send_json(504, {"error": "timeout"}); return
        self._send_json(200, result or {})

    def _send_json(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self._cors()
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def _process_camera_cmd(req):
    """Handle camera commands on the main thread."""
    global _render_result, _render_error, _preview_cache, _preview_dirty, _preview_cam_name, _preview_size
    import math

    action = req.get("action")
    scene = bpy.context.scene

    try:
        if action == "list_cameras":
            cameras = []
            for obj in scene.objects:
                if obj.type == "CAMERA":
                    cam = obj.data
                    cameras.append({
                        "name": obj.name,
                        "active": obj == scene.camera,
                        "focal_length": cam.lens,
                        "sensor_width": cam.sensor_width,
                        "clip_start": cam.clip_start,
                        "clip_end": cam.clip_end,
                        "location": list(obj.location),
                        "rotation": [math.degrees(r) for r in obj.rotation_euler],
                    })
            with _render_lock:
                _render_result = {"cameras": cameras}

        elif action == "set_active_camera":
            name = req.get("name", "")
            obj = scene.objects.get(name)
            if obj and obj.type == "CAMERA":
                scene.camera = obj
                _preview_dirty = True  # Invalidate preview cache.
                with _render_lock:
                    _render_result = {"ok": True, "active": name}
            else:
                with _render_lock:
                    _render_result = {"error": f"Camera '{name}' not found"}

        elif action == "camera_view":
            # Toggle viewport to camera view.
            enable = req.get("enable", True)
            for area in bpy.context.screen.areas:
                if area.type == "VIEW_3D":
                    for space in area.spaces:
                        if space.type == "VIEW_3D":
                            if enable:
                                space.region_3d.view_perspective = "CAMERA"
                            else:
                                space.region_3d.view_perspective = "PERSP"
                    break
            with _render_lock:
                _render_result = {"ok": True, "camera_view": enable}

        elif action == "camera_params":
            name = req.get("name", "")
            obj = scene.objects.get(name) if name else scene.camera
            if not obj or obj.type != "CAMERA":
                with _render_lock:
                    _render_result = {"error": "Camera not found"}
                return

            cam = obj.data
            # Set params if provided.
            if "focal_length" in req:
                cam.lens = req["focal_length"]
            if "sensor_width" in req:
                cam.sensor_width = req["sensor_width"]
            if "clip_start" in req:
                cam.clip_start = req["clip_start"]
            if "clip_end" in req:
                cam.clip_end = req["clip_end"]
            if "loc_x" in req:
                obj.location.x = req["loc_x"]
            if "loc_y" in req:
                obj.location.y = req["loc_y"]
            if "loc_z" in req:
                obj.location.z = req["loc_z"]
            if "rot_x" in req:
                obj.rotation_euler.x = math.radians(req["rot_x"])
            if "rot_y" in req:
                obj.rotation_euler.y = math.radians(req["rot_y"])
            if "rot_z" in req:
                obj.rotation_euler.z = math.radians(req["rot_z"])

            # Return current values.
            with _render_lock:
                _render_result = {
                    "name": obj.name,
                    "focal_length": cam.lens,
                    "sensor_width": cam.sensor_width,
                    "clip_start": cam.clip_start,
                    "clip_end": cam.clip_end,
                    "location": list(obj.location),
                    "rotation": [math.degrees(r) for r in obj.rotation_euler],
                }

        elif action == "scene_info":
            # Return animation and render settings.
            with _render_lock:
                _render_result = {
                    "frame_start": scene.frame_start,
                    "frame_end": scene.frame_end,
                    "frame_current": scene.frame_current,
                    "frame_step": scene.frame_step,
                    "fps": scene.render.fps,
                    "fps_base": scene.render.fps_base,
                    "resolution_x": scene.render.resolution_x,
                    "resolution_y": scene.render.resolution_y,
                    "engine": scene.render.engine,
                    "cameras": [o.name for o in scene.objects if o.type == "CAMERA"],
                    "active_camera": scene.camera.name if scene.camera else None,
                }

        elif action == "render_animation":
            # Render each frame and save to temp files.
            w = req["width"]
            h = req["height"]
            samples = req["samples"]
            engine = req["engine"]
            start = req["start"]
            end = req["end"]
            step = req["step"]
            cam_name = req.get("camera", "")

            # Save originals.
            orig = {
                "engine": scene.render.engine,
                "res_x": scene.render.resolution_x,
                "res_y": scene.render.resolution_y,
                "res_pct": scene.render.resolution_percentage,
                "filepath": scene.render.filepath,
                "format": scene.render.image_settings.file_format,
                "frame": scene.frame_current,
                "camera": scene.camera,
            }

            use_opengl = (engine == "OPENGL")
            frame_files = []
            try:
                # Set camera if specified.
                if cam_name:
                    cam_obj = scene.objects.get(cam_name)
                    if cam_obj and cam_obj.type == "CAMERA":
                        scene.camera = cam_obj

                scene.render.resolution_x = w
                scene.render.resolution_y = h
                scene.render.resolution_percentage = 100
                scene.render.image_settings.file_format = "PNG"

                if not use_opengl:
                    scene.render.engine = engine
                    if engine == "CYCLES" and hasattr(scene, "cycles"):
                        orig["cycles_samples"] = scene.cycles.samples
                        scene.cycles.samples = samples
                    elif hasattr(scene, "eevee"):
                        orig["eevee_samples"] = scene.eevee.taa_render_samples
                        scene.eevee.taa_render_samples = samples

                # Switch viewport to camera for OpenGL render.
                if use_opengl:
                    for area in bpy.context.screen.areas:
                        if area.type == "VIEW_3D":
                            for space in area.spaces:
                                if space.type == "VIEW_3D":
                                    orig["view_persp"] = space.region_3d.view_perspective
                                    space.region_3d.view_perspective = "CAMERA"
                            break

                total = len(range(start, end + 1, step))
                _log(f"Rendering animation: frames {start}-{end} step={step} ({total} frames) engine={'OPENGL' if use_opengl else engine}")

                for i, frame in enumerate(range(start, end + 1, step)):
                    scene.frame_set(frame)
                    tmp = tempfile.mktemp(suffix=f"_f{frame:04d}.png")
                    scene.render.filepath = tmp
                    if use_opengl:
                        bpy.ops.render.opengl(write_still=True)
                    else:
                        bpy.ops.render.render(write_still=True)
                    if os.path.exists(tmp):
                        frame_files.append(tmp)
                    _log(f"  Frame {frame} ({i+1}/{total}) done")

                with _render_lock:
                    _render_result = {"_frames": frame_files}
                _log(f"Animation render complete: {len(frame_files)} frames")

            except Exception as e_anim:
                _log(f"Animation render error: {e_anim}")
                with _render_lock:
                    _render_result = {"error": str(e_anim)}
            finally:
                scene.render.engine = orig["engine"]
                scene.render.resolution_x = orig["res_x"]
                scene.render.resolution_y = orig["res_y"]
                scene.render.resolution_percentage = orig["res_pct"]
                scene.render.filepath = orig["filepath"]
                scene.render.image_settings.file_format = orig["format"]
                scene.frame_set(orig["frame"])
                scene.camera = orig["camera"]
                if "cycles_samples" in orig and hasattr(scene, "cycles"):
                    scene.cycles.samples = orig["cycles_samples"]
                if "eevee_samples" in orig and hasattr(scene, "eevee"):
                    scene.eevee.taa_render_samples = orig["eevee_samples"]
                # Restore viewport perspective if changed for OpenGL.
                if "view_persp" in orig:
                    for area in bpy.context.screen.areas:
                        if area.type == "VIEW_3D":
                            for space in area.spaces:
                                if space.type == "VIEW_3D":
                                    space.region_3d.view_perspective = orig["view_persp"]
                            break

        elif action == "render_video":
            # Use Blender's native FFmpeg pipeline to render directly to video file.
            w = req["width"]
            h = req["height"]
            samples = req["samples"]
            engine = req["engine"]
            start = req["start"]
            end = req["end"]
            step = req["step"]
            cam_name = req.get("camera", "")
            fps = req.get("fps", 24)
            container = req.get("container", "MPEG4")
            codec = req.get("codec", "H264")
            quality = req.get("quality", "MEDIUM")
            video_bitrate = req.get("video_bitrate", 6000)
            audio_codec = req.get("audio_codec", "AAC")
            audio_bitrate = req.get("audio_bitrate", 192)
            output_path = req.get("output_path", "")
            film_transparent = req.get("film_transparent", False)

            # Map codec/container names to Blender RNA enum values.
            container_map = {
                "MPEG4": "MPEG4", "MKV": "MKV", "WEBM": "WEBM",
                "AVI": "AVI", "MOV": "QUICKTIME", "OGG": "OGG",
            }
            codec_map = {
                "H264": "H264", "H265": "H265", "AV1": "AV1",
                "VP9": "WEBM", "PRORES": "PRORES",
                "MPEG4": "MPEG4", "PNG": "PNG",
                "NONE": "NONE",
            }
            audio_codec_map = {
                "AAC": "AAC", "AC3": "AC3", "FLAC": "FLAC",
                "MP2": "MP2", "MP3": "MP3", "OPUS": "OPUS",
                "PCM": "PCM", "VORBIS": "VORBIS", "NONE": "NONE",
            }
            quality_map = {
                "NONE": "NONE", "LOSSLESS": "LOSSLESS",
                "PERC_LOSSLESS": "PERC_LOSSLESS",
                "HIGH": "HIGH", "MEDIUM": "MEDIUM",
                "LOW": "LOW", "VERYLOW": "VERYLOW", "LOWEST": "LOWEST",
            }

            # Generate output path if not specified.
            if not output_path:
                ext_map = {"MPEG4": ".mp4", "MKV": ".mkv", "WEBM": ".webm",
                           "AVI": ".avi", "QUICKTIME": ".mov", "OGG": ".ogv"}
                bpy_container = container_map.get(container, "MPEG4")
                ext = ext_map.get(bpy_container, ".mp4")
                output_path = os.path.join(tempfile.gettempdir(), f"blender_render{ext}")

            # Save originals.
            orig = {
                "engine": scene.render.engine,
                "res_x": scene.render.resolution_x,
                "res_y": scene.render.resolution_y,
                "res_pct": scene.render.resolution_percentage,
                "filepath": scene.render.filepath,
                "media_type": scene.render.image_settings.media_type,
                "format": scene.render.image_settings.file_format,
                "frame_start": scene.frame_start,
                "frame_end": scene.frame_end,
                "frame_step": scene.frame_step,
                "fps": scene.render.fps,
                "fps_base": scene.render.fps_base,
                "camera": scene.camera,
                "film_transparent": scene.render.film_transparent,
            }
            use_opengl = (engine == "OPENGL")
            orig_ff = {}
            try:
                # Save FFmpeg settings (only available with WITH_CODEC_FFMPEG).
                ffmpeg = scene.render.ffmpeg
                if hasattr(ffmpeg, "format"):
                    orig_ff = {
                        "format": ffmpeg.format,
                        "codec": ffmpeg.codec,
                        "constant_rate_factor": ffmpeg.constant_rate_factor,
                        "video_bitrate": ffmpeg.video_bitrate,
                        "audio_codec": ffmpeg.audio_codec,
                        "audio_bitrate": ffmpeg.audio_bitrate,
                        "gopsize": ffmpeg.gopsize,
                    }
                # Set camera.
                if cam_name:
                    cam_obj = scene.objects.get(cam_name)
                    if cam_obj and cam_obj.type == "CAMERA":
                        scene.camera = cam_obj

                # Set render settings.
                if not use_opengl:
                    scene.render.engine = engine
                scene.render.resolution_x = w
                scene.render.resolution_y = h
                scene.render.resolution_percentage = 100
                scene.render.fps = fps
                scene.render.fps_base = 1.0
                scene.render.film_transparent = film_transparent
                scene.frame_start = start
                scene.frame_end = end
                scene.frame_step = step

                if not use_opengl:
                    if engine == "CYCLES" and hasattr(scene, "cycles"):
                        orig["cycles_samples"] = scene.cycles.samples
                        scene.cycles.samples = samples
                    elif hasattr(scene, "eevee"):
                        orig["eevee_samples"] = scene.eevee.taa_render_samples
                        scene.eevee.taa_render_samples = samples

                # Set FFmpeg output — must set media_type to VIDEO first
                # so that FFMPEG becomes available in file_format enum.
                scene.render.image_settings.media_type = "VIDEO"
                scene.render.image_settings.file_format = "FFMPEG"
                bpy_container = container_map.get(container, "MPEG4")
                ffmpeg.format = bpy_container
                ffmpeg.codec = codec_map.get(codec, "H264")
                ffmpeg.constant_rate_factor = quality_map.get(quality, "MEDIUM")
                if quality == "NONE":
                    ffmpeg.video_bitrate = video_bitrate
                ffmpeg.audio_codec = audio_codec_map.get(audio_codec, "AAC")
                ffmpeg.audio_bitrate = audio_bitrate
                ffmpeg.gopsize = fps  # Keyframe every second.

                scene.render.filepath = output_path

                total = len(range(start, end + 1, step))
                _log(f"Rendering video: {w}x{h} {engine} frames {start}-{end} → {output_path}")
                _log(f"  FFmpeg: {bpy_container}/{codec} quality={quality} audio={audio_codec}")

                if use_opengl:
                    for area in bpy.context.screen.areas:
                        if area.type == "VIEW_3D":
                            for space in area.spaces:
                                if space.type == "VIEW_3D":
                                    from mathutils import Quaternion, Vector
                                    r3d = space.region_3d
                                    orig["view_persp"] = r3d.view_perspective
                                    orig["view_rotation"] = r3d.view_rotation.copy()
                                    orig["view_location"] = r3d.view_location.copy()
                                    orig["view_distance"] = r3d.view_distance
                                    r3d.view_perspective = "CAMERA"
                            break
                    bpy.ops.render.opengl(animation=True)
                else:
                    bpy.ops.render.render(animation=True)

                # Find the actual output file (Blender adds extension).
                video_file = output_path
                if not os.path.exists(video_file):
                    # Try common extensions.
                    for ext in [".mp4", ".mkv", ".webm", ".avi", ".mov", ".ogv"]:
                        candidate = output_path + ext
                        if os.path.exists(candidate):
                            video_file = candidate
                            break

                if os.path.exists(video_file):
                    file_size = os.path.getsize(video_file)
                    with _render_lock:
                        _render_result = {
                            "video_path": video_file,
                            "frames": total,
                            "size_bytes": file_size,
                        }
                    _log(f"Video render complete: {video_file} ({file_size} bytes)")
                else:
                    with _render_lock:
                        _render_result = {"error": f"Video file not found at {output_path}"}

            except Exception as e_vid:
                _log(f"Video render error: {e_vid}")
                import traceback
                _log(traceback.format_exc())
                with _render_lock:
                    _render_result = {"error": str(e_vid)}
            finally:
                scene.render.engine = orig["engine"]
                scene.render.resolution_x = orig["res_x"]
                scene.render.resolution_y = orig["res_y"]
                scene.render.resolution_percentage = orig["res_pct"]
                scene.render.filepath = orig["filepath"]
                scene.render.image_settings.media_type = orig["media_type"]
                scene.render.image_settings.file_format = orig["format"]
                scene.frame_start = orig["frame_start"]
                scene.frame_end = orig["frame_end"]
                scene.frame_step = orig["frame_step"]
                scene.render.fps = orig["fps"]
                scene.render.fps_base = orig["fps_base"]
                scene.camera = orig["camera"]
                scene.render.film_transparent = orig["film_transparent"]
                if "cycles_samples" in orig and hasattr(scene, "cycles"):
                    scene.cycles.samples = orig["cycles_samples"]
                if "eevee_samples" in orig and hasattr(scene, "eevee"):
                    scene.eevee.taa_render_samples = orig["eevee_samples"]
                # Restore FFmpeg settings.
                if orig_ff:
                    ffmpeg = scene.render.ffmpeg
                    ffmpeg.format = orig_ff["format"]
                    ffmpeg.codec = orig_ff["codec"]
                    ffmpeg.constant_rate_factor = orig_ff["constant_rate_factor"]
                    ffmpeg.video_bitrate = orig_ff["video_bitrate"]
                    ffmpeg.audio_codec = orig_ff["audio_codec"]
                    ffmpeg.audio_bitrate = orig_ff["audio_bitrate"]
                    ffmpeg.gopsize = orig_ff["gopsize"]
                # Restore viewport.
                if "view_persp" in orig:
                    for area in bpy.context.screen.areas:
                        if area.type == "VIEW_3D":
                            for space in area.spaces:
                                if space.type == "VIEW_3D":
                                    r3d = space.region_3d
                                    r3d.view_perspective = orig["view_persp"]
                                    if "view_rotation" in orig:
                                        r3d.view_rotation = orig["view_rotation"]
                                        r3d.view_location = orig["view_location"]
                                        r3d.view_distance = orig["view_distance"]
                            break

            _render_event.set()

        elif action == "opengl_preview":
            # Offscreen render from active camera — does NOT touch the viewport.
            # Uses cache: only re-renders when depsgraph marks dirty.
            import gpu

            w = req.get("width", 640)
            h = req.get("height", 480)

            cam_obj = scene.camera
            cam_name = cam_obj.name if cam_obj else ""

            # Return cached preview if scene hasn't changed.
            if (not _preview_dirty
                    and _preview_cache is not None
                    and _preview_cam_name == cam_name
                    and _preview_size == (w, h)):
                with _render_lock:
                    _render_result = {"_bytes": _preview_cache, "_content_type": "image/bmp"}
                _render_event.set()
                return
            if not cam_obj:
                with _render_lock:
                    _render_result = {"error": "No active camera"}
                _render_event.set()
                return

            # Get camera matrices.
            depsgraph = bpy.context.evaluated_depsgraph_get()
            view_matrix = cam_obj.matrix_world.inverted()
            projection_matrix = cam_obj.calc_matrix_camera(depsgraph, x=w, y=h)

            # Find a VIEW_3D space and region for draw_view3d.
            space_v3d = None
            region = None
            for area in bpy.context.screen.areas:
                if area.type == "VIEW_3D":
                    space_v3d = area.spaces[0]
                    for r in area.regions:
                        if r.type == "WINDOW":
                            region = r
                            break
                    break

            if not space_v3d or not region:
                with _render_lock:
                    _render_result = {"error": "No 3D viewport found"}
                _render_event.set()
                return

            try:
                offscreen = gpu.types.GPUOffScreen(w, h)
                with offscreen.bind():
                    offscreen.draw_view3d(
                        scene,
                        bpy.context.view_layer,
                        space_v3d,
                        region,
                        view_matrix,
                        projection_matrix,
                    )
                    # Read pixels via active framebuffer.
                    fb = gpu.state.active_framebuffer_get()
                    pixel_buf = fb.read_color(0, 0, w, h, 4, 0, 'UBYTE')
                    pixel_buf.dimensions = w * h * 4

                offscreen.free()

                # Build BMP in memory (no disk I/O).
                raw = bytes(pixel_buf)
                import struct
                row_pad = (4 - (w * 3) % 4) % 4
                pixel_size = (w * 3 + row_pad) * h
                file_size = 54 + pixel_size

                bmp = bytearray(file_size)
                struct.pack_into('<2sIHHI', bmp, 0, b'BM', file_size, 0, 0, 54)
                struct.pack_into('<IiiHHIIiiII', bmp, 14,
                                40, w, h, 1, 24, 0, pixel_size, 0, 0, 0, 0)
                dst = 54
                for y in range(h):
                    src_off = y * w * 4
                    for x in range(w):
                        s = src_off + x * 4
                        bmp[dst] = raw[s + 2]
                        bmp[dst + 1] = raw[s + 1]
                        bmp[dst + 2] = raw[s]
                        dst += 3
                    dst += row_pad

                bmp_bytes = bytes(bmp)
                _preview_cache = bmp_bytes
                _preview_dirty = False
                _preview_cam_name = cam_name
                _preview_size = (w, h)

                with _render_lock:
                    _render_result = {"_bytes": bmp_bytes, "_content_type": "image/bmp"}

            except Exception as e2:
                _log(f"Offscreen preview error: {e2}")
                import traceback
                _log(traceback.format_exc())
                with _render_lock:
                    _render_result = {"error": str(e2)}

    except Exception as e:
        _log(f"Camera cmd error: {e}")
        with _render_lock:
            _render_result = {"error": str(e)}

    _render_event.set()


def _process_render():
    """Called by bpy.app.timers on the main thread."""
    global _render_request, _render_result, _render_error

    with _render_lock:
        req = _render_request
        _render_request = None

    if req is None:
        return 0.1

    # Dispatch camera commands.
    if "action" in req:
        _process_camera_cmd(req)
        return 0.1

    _log(f"Processing render: {req}")

    scene = bpy.context.scene

    # Save original settings.
    orig = {
        "engine": scene.render.engine,
        "res_x": scene.render.resolution_x,
        "res_y": scene.render.resolution_y,
        "res_pct": scene.render.resolution_percentage,
        "filepath": scene.render.filepath,
        "format": scene.render.image_settings.file_format,
    }

    use_opengl = (req["engine"] == "OPENGL")

    try:
        # Apply settings.
        if not use_opengl:
            scene.render.engine = req["engine"]
        scene.render.resolution_x = req["width"]
        scene.render.resolution_y = req["height"]
        scene.render.resolution_percentage = 100

        if not use_opengl:
            if req["engine"] == "CYCLES" and hasattr(scene, "cycles"):
                orig["cycles_samples"] = scene.cycles.samples
                scene.cycles.samples = req["samples"]
            elif hasattr(scene, "eevee"):
                orig["eevee_samples"] = scene.eevee.taa_render_samples
                scene.eevee.taa_render_samples = req["samples"]

        tmp = tempfile.mktemp(suffix=".png")
        scene.render.filepath = tmp
        scene.render.image_settings.file_format = "PNG"

        if use_opengl:
            # Switch viewport to camera view for OpenGL render.
            # Save full view state so we can restore exactly after render.
            for area in bpy.context.screen.areas:
                if area.type == "VIEW_3D":
                    for space in area.spaces:
                        if space.type == "VIEW_3D":
                            r3d = space.region_3d
                            from mathutils import Quaternion, Vector
                            orig["view_persp"] = r3d.view_perspective
                            orig["view_rotation"] = r3d.view_rotation.copy()
                            orig["view_location"] = r3d.view_location.copy()
                            orig["view_distance"] = r3d.view_distance
                            orig["view_camera_offset"] = (r3d.view_camera_offset[0], r3d.view_camera_offset[1])
                            orig["view_camera_zoom"] = r3d.view_camera_zoom
                            r3d.view_perspective = "CAMERA"
                    break
            _log("Calling bpy.ops.render.opengl...")
            result = bpy.ops.render.opengl(write_still=True)
        else:
            _log("Calling bpy.ops.render.render...")
            result = bpy.ops.render.render(write_still=True)
        _log(f"Render result: {result}")

        if os.path.exists(tmp):
            with _render_lock:
                _render_result = tmp
        else:
            with _render_lock:
                _render_error = f"Render completed ({result}) but no output file"

    except Exception as e:
        tb = traceback.format_exc()
        _log(f"Render error: {e}\n{tb}")
        with _render_lock:
            _render_error = str(e)
    finally:
        # Restore original settings.
        scene.render.engine = orig["engine"]
        scene.render.resolution_x = orig["res_x"]
        scene.render.resolution_y = orig["res_y"]
        scene.render.resolution_percentage = orig["res_pct"]
        scene.render.filepath = orig["filepath"]
        scene.render.image_settings.file_format = orig["format"]

        if "cycles_samples" in orig and hasattr(scene, "cycles"):
            scene.cycles.samples = orig["cycles_samples"]
        if "eevee_samples" in orig and hasattr(scene, "eevee"):
            scene.eevee.taa_render_samples = orig["eevee_samples"]
        # Restore full viewport state if changed for OpenGL render.
        if "view_persp" in orig:
            for area in bpy.context.screen.areas:
                if area.type == "VIEW_3D":
                    for space in area.spaces:
                        if space.type == "VIEW_3D":
                            r3d = space.region_3d
                            r3d.view_perspective = orig["view_persp"]
                            r3d.view_rotation = orig["view_rotation"]
                            r3d.view_location = orig["view_location"]
                            r3d.view_distance = orig["view_distance"]
                            r3d.view_camera_offset = orig["view_camera_offset"]
                            r3d.view_camera_zoom = orig["view_camera_zoom"]
                    break

        _render_event.set()

    return 0.1


def _start_server():
    try:
        server = HTTPServer((RENDER_BIND, RENDER_PORT), RenderHandler)
        server.daemon_threads = True
        _log(f"Render API on http://{RENDER_BIND}:{RENDER_PORT}")
        server.serve_forever()
    except Exception as e:
        _log(f"Failed to start server: {e}")


@bpy.app.handlers.persistent
def _on_depsgraph_update(scene, depsgraph):
    """Mark preview dirty when anything in the scene changes."""
    global _preview_dirty
    _preview_dirty = True


@bpy.app.handlers.persistent
def _on_frame_change(scene, depsgraph):
    """Mark preview dirty on frame change (animation)."""
    global _preview_dirty
    _preview_dirty = True


def register():
    t = threading.Thread(target=_start_server, daemon=True)
    t.start()
    bpy.app.timers.register(_process_render, persistent=True)
    bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph_update)
    bpy.app.handlers.frame_change_post.append(_on_frame_change)
    _log("Registered")


def unregister():
    try:
        bpy.app.timers.unregister(_process_render)
    except ValueError:
        pass
    if _on_depsgraph_update in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph_update)
    if _on_frame_change in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.remove(_on_frame_change)


if __name__ != "__main__":
    register()

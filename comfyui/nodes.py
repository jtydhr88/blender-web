import io
import json
import os
import torch
import numpy as np
from PIL import Image, ImageOps
import folder_paths
import urllib.request
import urllib.parse


def _api_url(server_url):
    """Return (ws_host, ws_port, api_port) from server_url."""
    from urllib.parse import urlparse
    parsed = urlparse(server_url)
    host = parsed.hostname or "localhost"
    ws_port = parsed.port or 7681
    return host, ws_port, ws_port + 1


class BlenderRenderSettings:
    """Render engine, resolution, and quality settings."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {
                "width": ("INT", {"default": 1920, "min": 1, "max": 8192, "step": 1}),
                "height": ("INT", {"default": 1080, "min": 1, "max": 8192, "step": 1}),
                "samples": ("INT", {"default": 32, "min": 1, "max": 4096, "step": 1}),
                "engine": (["OPENGL", "BLENDER_EEVEE", "CYCLES"],),
                "film_transparent": ("BOOLEAN", {"default": False}),
            },
        }

    RETURN_TYPES = ("BLENDER_RENDER_SETTINGS",)
    RETURN_NAMES = ("render_settings",)
    FUNCTION = "build"
    CATEGORY = "Blender"

    def build(self, width=1920, height=1080, samples=32, engine="BLENDER_EEVEE",
              film_transparent=False):
        return ({"width": width, "height": height, "samples": samples,
                 "engine": engine, "film_transparent": film_transparent},)


class BlenderFFmpegSettings:
    """Video encoding settings — container, codec, quality, audio."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {
                "container": (["MPEG4", "MKV", "WEBM", "AVI", "MOV", "OGG"],),
                "codec": (["H264", "H265", "AV1", "VP9", "PRORES", "MPEG4", "PNG", "NONE"],),
                "quality": (["HIGH", "MEDIUM", "LOW", "PERC_LOSSLESS", "LOSSLESS",
                             "VERYLOW", "LOWEST", "NONE"],),
                "video_bitrate": ("INT", {"default": 6000, "min": 100, "max": 100000, "step": 100}),
                "audio_codec": (["AAC", "AC3", "FLAC", "MP3", "OPUS", "PCM", "VORBIS", "NONE"],),
                "audio_bitrate": ("INT", {"default": 192, "min": 32, "max": 2048, "step": 32}),
                "fps": ("INT", {"default": 24, "min": 1, "max": 240, "step": 1}),
            },
            "optional": {
                "output_path": ("STRING", {"default": ""}),
            },
        }

    RETURN_TYPES = ("BLENDER_FFMPEG_SETTINGS",)
    RETURN_NAMES = ("ffmpeg_settings",)
    FUNCTION = "build"
    CATEGORY = "Blender"

    def build(self, container="MPEG4", codec="H264", quality="MEDIUM",
              video_bitrate=6000, audio_codec="AAC", audio_bitrate=192,
              fps=24, output_path=""):
        return ({
            "container": container, "codec": codec, "quality": quality,
            "video_bitrate": video_bitrate, "audio_codec": audio_codec,
            "audio_bitrate": audio_bitrate, "fps": fps,
            "output_path": output_path,
        },)


class BlenderAnimationSettings:
    """Animation frame range and camera selection."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {
                "start_frame": ("INT", {"default": 1, "min": 0, "max": 100000, "step": 1}),
                "end_frame": ("INT", {"default": 60, "min": 1, "max": 100000, "step": 1}),
                "frame_step": ("INT", {"default": 1, "min": 1, "max": 100, "step": 1}),
                "camera_name": ("STRING", {"default": "Camera"}),
            },
        }

    RETURN_TYPES = ("BLENDER_ANIMATION_SETTINGS",)
    RETURN_NAMES = ("animation_settings",)
    FUNCTION = "build"
    CATEGORY = "Blender"

    def build(self, start_frame=1, end_frame=60, frame_step=1, camera_name="Camera"):
        return ({
            "start": start_frame, "end": end_frame, "step": frame_step,
            "camera": camera_name,
        },)


class BlenderWebDisplay:
    """Blender viewport in ComfyUI. Without render_settings, does a fast
    OpenGL camera capture. With render_settings, does a full render."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {},
            "optional": {
                "render_settings": ("BLENDER_RENDER_SETTINGS",),
                "server_url": ("STRING", {"default": "http://localhost:7681"}),
            },
            "hidden": {
                "image": ("STRING", {"default": ""}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("image",)
    FUNCTION = "run"
    OUTPUT_NODE = True
    CATEGORY = "Blender"

    def run(self, render_settings=None, server_url="http://localhost:7681",
            image="", **kwargs):
        host, ws_port, api_port = _api_url(server_url)

        if render_settings:
            rs = render_settings
            render_url = (
                f"http://{host}:{api_port}/render"
                f"?width={rs['width']}&height={rs['height']}"
                f"&samples={rs['samples']}&engine={rs['engine']}"
            )
        else:
            render_url = (
                f"http://{host}:{api_port}/render"
                f"?width=1280&height=720&samples=1&engine=OPENGL"
            )

        try:
            req = urllib.request.Request(render_url, headers={"User-Agent": "ComfyUI"})
            with urllib.request.urlopen(req, timeout=180) as resp:
                png_data = resp.read()
        except Exception as e:
            raise RuntimeError(f"Blender render failed: {e}")

        img = Image.open(io.BytesIO(png_data)).convert("RGB")
        img_np = np.array(img).astype(np.float32) / 255.0
        return (torch.from_numpy(img_np).unsqueeze(0),)


class BlenderCameraPreview:
    """Fast camera preview — captures the live viewport from camera angle."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {
                "server_url": ("STRING", {"default": "http://localhost:7681"}),
                "camera_name": ("STRING", {"default": "Camera"}),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("preview",)
    FUNCTION = "capture_preview"
    CATEGORY = "Blender"

    def capture_preview(self, server_url="http://localhost:7681", camera_name="Camera"):
        import time
        host, ws_port, api_port = _api_url(server_url)

        if camera_name:
            try:
                set_url = f"http://{host}:{api_port}/camera/active?name={urllib.parse.quote(camera_name)}"
                urllib.request.urlopen(set_url, timeout=5)
            except Exception:
                pass
        try:
            urllib.request.urlopen(f"http://{host}:{api_port}/camera/view?enable=1", timeout=5)
        except Exception:
            pass

        time.sleep(0.15)

        capture_url = f"http://{host}:{ws_port}/capture"
        try:
            req = urllib.request.Request(capture_url, headers={"User-Agent": "ComfyUI"})
            with urllib.request.urlopen(req, timeout=10) as resp:
                jpeg_data = resp.read()
        except Exception as e:
            raise RuntimeError(f"Blender viewport capture failed: {e}")

        img = Image.open(io.BytesIO(jpeg_data)).convert("RGB")
        img_np = np.array(img).astype(np.float32) / 255.0
        return (torch.from_numpy(img_np).unsqueeze(0),)


class BlenderRenderVideo:
    """Render Blender animation directly to video file using native FFmpeg pipeline.
    Outputs the video file path and optionally loads first frame as preview."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {
                "server_url": ("STRING", {"default": "http://localhost:7681"}),
                "animation_settings": ("BLENDER_ANIMATION_SETTINGS",),
                "ffmpeg_settings": ("BLENDER_FFMPEG_SETTINGS",),
            },
            "optional": {
                "render_settings": ("BLENDER_RENDER_SETTINGS",),
            },
        }

    RETURN_TYPES = ("STRING", "IMAGE",)
    RETURN_NAMES = ("video_path", "preview",)
    FUNCTION = "render_video"
    OUTPUT_NODE = True
    CATEGORY = "Blender"

    def render_video(self, server_url="http://localhost:7681",
                     animation_settings=None, ffmpeg_settings=None,
                     render_settings=None):
        if not animation_settings:
            raise RuntimeError("No animation settings provided")
        if not ffmpeg_settings:
            raise RuntimeError("No FFmpeg settings provided")

        host, ws_port, api_port = _api_url(server_url)

        anim = animation_settings
        ff = ffmpeg_settings
        rs = render_settings or {"width": 1920, "height": 1080, "samples": 32,
                                  "engine": "BLENDER_EEVEE", "film_transparent": False}

        params = urllib.parse.urlencode({
            "width": rs["width"], "height": rs["height"],
            "samples": rs["samples"], "engine": rs["engine"],
            "film_transparent": "1" if rs.get("film_transparent") else "0",
            "start": anim["start"], "end": anim["end"], "step": anim["step"],
            "camera": anim.get("camera", ""),
            "fps": ff["fps"],
            "container": ff["container"], "codec": ff["codec"],
            "quality": ff["quality"], "video_bitrate": ff["video_bitrate"],
            "audio_codec": ff["audio_codec"], "audio_bitrate": ff["audio_bitrate"],
            "output_path": ff.get("output_path", ""),
        })
        url = f"http://{host}:{api_port}/render/video?{params}"

        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ComfyUI"})
            with urllib.request.urlopen(req, timeout=3660) as resp:
                result = json.loads(resp.read())
        except Exception as e:
            raise RuntimeError(f"Blender video render failed: {e}")

        if "error" in result:
            raise RuntimeError(f"Blender error: {result['error']}")

        video_path = result.get("video_path", "")
        if not video_path or not os.path.exists(video_path):
            raise RuntimeError(f"Video file not found: {video_path}")

        # Return 1x1 black as preview placeholder (video is the main output).
        preview = torch.zeros(1, 64, 64, 3)

        return (video_path, preview,)


class BlenderAnimationRender:
    """Render animation frames and output as IMAGE batch (for per-frame processing)."""

    @classmethod
    def INPUT_TYPES(s):
        return {
            "required": {
                "server_url": ("STRING", {"default": "http://localhost:7681"}),
                "animation_settings": ("BLENDER_ANIMATION_SETTINGS",),
            },
            "optional": {
                "render_settings": ("BLENDER_RENDER_SETTINGS",),
            },
        }

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("frames",)
    FUNCTION = "render_animation"
    CATEGORY = "Blender"

    def render_animation(self, server_url="http://localhost:7681",
                         animation_settings=None, render_settings=None):
        if not animation_settings:
            raise RuntimeError("No animation settings provided")

        host, ws_port, api_port = _api_url(server_url)

        anim = animation_settings
        rs = render_settings or {"width": 1024, "height": 1024, "samples": 32,
                                  "engine": "BLENDER_EEVEE"}

        params = urllib.parse.urlencode({
            "width": rs["width"], "height": rs["height"],
            "samples": rs["samples"], "engine": rs["engine"],
            "start": anim["start"], "end": anim["end"], "step": anim["step"],
            "camera": anim.get("camera", ""),
        })
        url = f"http://{host}:{api_port}/render/animation?{params}"

        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ComfyUI"})
            with urllib.request.urlopen(req, timeout=660) as resp:
                result = json.loads(resp.read())
        except Exception as e:
            raise RuntimeError(f"Blender animation render failed: {e}")

        if "error" in result:
            raise RuntimeError(f"Blender error: {result['error']}")

        frame_files = result.get("frames", [])
        if not frame_files:
            raise RuntimeError("No frames rendered")

        tensors = []
        for fpath in frame_files:
            if not os.path.exists(fpath):
                continue
            img = Image.open(fpath).convert("RGB")
            img_np = np.array(img).astype(np.float32) / 255.0
            tensors.append(torch.from_numpy(img_np))
            try:
                os.unlink(fpath)
            except OSError:
                pass

        if not tensors:
            raise RuntimeError("Failed to load rendered frames")

        batch = torch.stack(tensors, dim=0)
        return (batch,)


NODE_CLASS_MAPPINGS = {
    "BlenderRenderSettings": BlenderRenderSettings,
    "BlenderFFmpegSettings": BlenderFFmpegSettings,
    "BlenderAnimationSettings": BlenderAnimationSettings,
    "BlenderWebDisplay": BlenderWebDisplay,
    "BlenderCameraPreview": BlenderCameraPreview,
    "BlenderRenderVideo": BlenderRenderVideo,
    "BlenderAnimationRender": BlenderAnimationRender,
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "BlenderRenderSettings": "Blender Render Settings",
    "BlenderFFmpegSettings": "Blender FFmpeg Settings",
    "BlenderAnimationSettings": "Blender Animation Settings",
    "BlenderWebDisplay": "Blender Web Display",
    "BlenderCameraPreview": "Blender Camera Preview",
    "BlenderRenderVideo": "Blender Render Video",
    "BlenderAnimationRender": "Blender Animation Render",
}

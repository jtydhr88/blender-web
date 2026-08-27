# ComfyTV bridge

A JSON job API served from inside blender-web, purpose-built for
[ComfyTV](https://github.com/jtydhr88)'s runner model (probe → submit job →
poll), replacing the older blocking-GET ComfyUI plugin API.

Ships in release zips as `blender_comfytv_bridge.py` + `blender-for-comfytv.bat`.
The integration follows the Eagle pattern: the user launches this distribution
themselves; ComfyTV's Blender stages probe `http://127.0.0.1:7684/comfytv/status`
and light up when it is running.

| Endpoint | Method | Purpose |
|---|---|---|
| `/comfytv/status` | GET | identity + scene summary (probe target) |
| `/comfytv/import` | POST | import GLB/OBJ/FBX/USD, optional scene clear |
| `/comfytv/camera/orbit` | POST | build an orbit camera rig around the mesh bounds |
| `/comfytv/render` | POST | start a still/video render job → `{job_id}` |
| `/comfytv/jobs/<id>` | GET | poll job status/progress/result |

Renders run with `INVOKE_DEFAULT`, so the streamed UI stays live and shows
progress. Port 7684 is localhost-only — same trust model as the web port.

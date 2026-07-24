import { app } from "../../../scripts/app.js";
import { api } from "../../../scripts/api.js";

/* ---- Blender WebSocket client embedded directly in the node ---- */

function createBlenderClient(container, serverUrl) {
  const canvas = document.createElement("canvas");
  canvas.style.cssText = "width:100%;height:100%;display:block;background:#1d1d1d;cursor:default;";
  canvas.dataset.blockLitegraph = "true";
  container.appendChild(canvas);
  const ctx = canvas.getContext("2d");

  const status = document.createElement("div");
  status.style.cssText =
    "position:absolute;top:4px;right:4px;color:#888;font:11px monospace;background:rgba(0,0,0,0.6);padding:2px 6px;border-radius:3px;pointer-events:none;z-index:1;";
  status.textContent = "Connecting...";
  container.appendChild(status);

  let ws = null;
  let frameCount = 0;
  let lastFpsTime = Date.now();
  let reconnectTimer = null;

  function connect() {
    if (ws) { try { ws.close(); } catch(e) {} }
    const wsUrl = serverUrl.replace(/^http/, "ws").replace(/\/$/, "") + "/ws";
    try {
      ws = new WebSocket(wsUrl);
    } catch(e) {
      status.textContent = "WS error";
      scheduleReconnect();
      return;
    }
    ws.binaryType = "blob";

    ws.onopen = function () {
      status.textContent = "Connected";
      status.style.color = "#8f8";
      send({ type: "resize", width: canvas.clientWidth, height: canvas.clientHeight });
    };

    ws.onclose = function () {
      status.textContent = "Disconnected";
      status.style.color = "#888";
      scheduleReconnect();
    };

    ws.onerror = function () {
      try { ws.close(); } catch(e) {}
    };

    ws.onmessage = function (evt) {
      if (evt.data instanceof Blob) {
        createImageBitmap(evt.data).then(function (bmp) {
          if (canvas.width !== bmp.width || canvas.height !== bmp.height) {
            canvas.width = bmp.width;
            canvas.height = bmp.height;
          }
          ctx.drawImage(bmp, 0, 0);
          bmp.close();
          frameCount++;
          const now = Date.now();
          if (now - lastFpsTime >= 1000) {
            status.textContent = frameCount + " fps";
            frameCount = 0;
            lastFpsTime = now;
          }
        });
      }
    };
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => { reconnectTimer = null; connect(); }, 2000);
  }

  function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  /* --- Input event handlers ---
   * Use capture phase (3rd arg = true) to intercept BEFORE ComfyUI/LiteGraph. */
  function stop(e) { e.preventDefault(); e.stopPropagation(); e.stopImmediatePropagation(); }

  canvas.addEventListener("mousemove", (e) => { send({ type: "mousemove", x: e.offsetX, y: e.offsetY }); }, true);
  canvas.addEventListener("mousedown", (e) => { stop(e); send({ type: "mousedown", button: e.button, x: e.offsetX, y: e.offsetY }); }, true);
  canvas.addEventListener("mouseup", (e) => { stop(e); send({ type: "mouseup", button: e.button, x: e.offsetX, y: e.offsetY }); }, true);
  canvas.addEventListener("wheel", (e) => { stop(e); send({ type: "wheel", deltaX: e.deltaX, deltaY: e.deltaY, x: e.offsetX, y: e.offsetY }); }, { capture: true, passive: false });
  canvas.addEventListener("contextmenu", (e) => stop(e), true);
  /* Block pointer events from reaching the LiteGraph canvas.
   * Use setPointerCapture to lock the pointer to our canvas during drag,
   * preventing LiteGraph from seeing middle-button pan events. */
  canvas.addEventListener("pointerdown", (e) => {
    e.stopPropagation(); e.stopImmediatePropagation();
    canvas.setPointerCapture(e.pointerId);
  }, true);
  canvas.addEventListener("pointermove", (e) => { e.stopPropagation(); e.stopImmediatePropagation(); }, true);
  canvas.addEventListener("pointerup", (e) => {
    e.stopPropagation(); e.stopImmediatePropagation();
    canvas.releasePointerCapture(e.pointerId);
  }, true);
  canvas.addEventListener("gotpointercapture", (e) => { e.stopPropagation(); }, true);
  canvas.addEventListener("lostpointercapture", (e) => { e.stopPropagation(); }, true);

  /* Keyboard: capture when canvas is focused. */
  canvas.tabIndex = 0;
  canvas.addEventListener("keydown", (e) => {
    if (e.key === "F12") return;
    e.preventDefault(); e.stopPropagation();
    send({ type: "keydown", key: e.key, code: e.code, repeat: e.repeat, shiftKey: e.shiftKey, ctrlKey: e.ctrlKey, altKey: e.altKey, metaKey: e.metaKey });
  });
  canvas.addEventListener("keyup", (e) => {
    e.preventDefault(); e.stopPropagation();
    send({ type: "keyup", key: e.key, code: e.code, repeat: false, shiftKey: e.shiftKey, ctrlKey: e.ctrlKey, altKey: e.altKey, metaKey: e.metaKey });
  });

  /* Focus canvas on click so keyboard works. */
  canvas.addEventListener("mousedown", () => canvas.focus());

  /* Resize observer. */
  const ro = new ResizeObserver(() => {
    send({ type: "resize", width: canvas.clientWidth, height: canvas.clientHeight });
  });
  ro.observe(canvas);

  connect();

  return { canvas, destroy: () => { if (ws) ws.close(); if (reconnectTimer) clearTimeout(reconnectTimer); ro.disconnect(); } };
}

/* ---- Upload capture as temp image ---- */

async function captureAndUpload(serverUrl) {
  const url = "/blender-web/capture?server=" + encodeURIComponent(serverUrl) + "&t=" + Date.now();
  const resp = await fetch(url);
  if (!resp.ok) throw new Error("Capture failed: " + resp.status);
  const blob = await resp.blob();

  const name = "blender_" + Date.now() + ".jpg";
  const file = new File([blob], name, { type: "image/jpeg" });
  const body = new FormData();
  body.append("image", file);
  body.append("subfolder", "blender_web");
  body.append("type", "temp");
  const uploadResp = await api.fetchApi("/upload/image", { method: "POST", body });
  if (uploadResp.status !== 200) throw new Error("Upload failed");
  return await uploadResp.json();
}

/* ---- Node widget ---- */

function createBlenderWidget(node) {
  let serverUrl = "http://localhost:7681";

  /* Server URL widget. */
  node._blenderUrlWidget = node.addWidget("text", "server_url", serverUrl, (v) => { serverUrl = v; });

  /* Canvas container. */
  const container = document.createElement("div");
  container.style.cssText = "width:100%;height:100%;position:relative;overflow:hidden;";

  const client = createBlenderClient(container, serverUrl);

  node.addDOMWidget("blender_view", "blender-display", container, {
    getMinHeight: () => 450,
    hideOnZoom: false,
    serialize: false,
  });

  /* Hidden image widget — captures frame on Queue. */
  const imageWidget = node.addWidget("text", "image", "", () => {});
  imageWidget.type = "hidden";
  imageWidget.serialize = true;
  imageWidget.serializeValue = async () => {
    try {
      const url = node._blenderUrlWidget?.value || serverUrl;
      const result = await captureAndUpload(url);
      return "blender_web/" + result.name + " [temp]";
    } catch (err) {
      console.error("Blender capture failed:", err);
      return "";
    }
  };

  /* Fullscreen button. */
  node.addWidget("button", "Fullscreen", null, () => {
    const url = node._blenderUrlWidget?.value || serverUrl;
    const w = Math.round(screen.width * 0.8), h = Math.round(screen.height * 0.8);
    window.open(url, "BlenderWeb", "width=" + w + ",height=" + h + ",resizable=yes");
  });

  node.setSize([Math.max(node.size[0], 500), Math.max(node.size[1], 700)]);
}

/* ---- Camera Preview Node ---- */

function createCameraPreviewWidget(node) {
  let serverUrl = "http://localhost:7681";
  let polling = true;
  let pollInterval = 200; /* 200ms polling, server returns cached if no change */

  /* --- Camera selector toolbar --- */
  const toolbar = document.createElement("div");
  toolbar.style.cssText = "display:flex;gap:4px;padding:4px;background:#2a2a2a;border-radius:4px 4px 0 0;align-items:center;";

  const cameraSelect = document.createElement("select");
  cameraSelect.style.cssText = "flex:1;padding:3px 6px;background:#383838;color:#ddd;border:1px solid #555;border-radius:3px;font-size:12px;";
  const defaultOpt = document.createElement("option");
  defaultOpt.textContent = "Loading cameras...";
  cameraSelect.appendChild(defaultOpt);
  toolbar.appendChild(cameraSelect);

  const refreshBtn = document.createElement("button");
  refreshBtn.textContent = "\u21BB"; /* ↻ */
  refreshBtn.title = "Refresh camera list";
  refreshBtn.style.cssText = "padding:3px 8px;background:#383838;color:#ddd;border:1px solid #555;border-radius:3px;cursor:pointer;font-size:14px;";
  refreshBtn.onclick = () => fetchCameras();
  toolbar.appendChild(refreshBtn);

  /* --- Preview image --- */
  const container = document.createElement("div");
  container.style.cssText = "width:100%;position:relative;overflow:hidden;background:#1a1a1a;border-radius:0 0 4px 4px;";

  const img = document.createElement("img");
  img.style.cssText = "width:100%;display:block;min-height:200px;";
  container.appendChild(img);

  const label = document.createElement("div");
  label.style.cssText = "position:absolute;bottom:4px;left:4px;color:#aaa;font:10px monospace;background:rgba(0,0,0,0.6);padding:2px 6px;border-radius:3px;pointer-events:none;";
  label.textContent = "Camera Preview";
  container.appendChild(label);

  /* --- Wrap toolbar + preview --- */
  const wrapper = document.createElement("div");
  wrapper.style.cssText = "width:100%;";
  wrapper.appendChild(toolbar);
  wrapper.appendChild(container);

  node.addDOMWidget("camera_preview", "camera-preview", wrapper, {
    getMinHeight: () => 280,
    hideOnZoom: false,
    serialize: false,
  });

  /* --- Helpers --- */
  function getServerUrl() {
    return node.widgets?.find((w) => w.name === "server_url")?.value || serverUrl;
  }

  function getApiBase() {
    try {
      const url = new URL(getServerUrl());
      return "http://" + (url.hostname || "localhost") + ":" + (parseInt(url.port || "7681") + 1);
    } catch (e) {
      return "http://localhost:7682";
    }
  }

  /* --- Fetch camera list from Blender --- */
  function fetchCameras() {
    const url = "/blender-web/cameras?server=" + encodeURIComponent(getServerUrl());
    fetch(url)
      .then((r) => r.json())
      .then((data) => {
        const cameras = data.cameras || [];
        cameraSelect.innerHTML = "";
        if (cameras.length === 0) {
          const opt = document.createElement("option");
          opt.textContent = "No cameras found";
          cameraSelect.appendChild(opt);
          return;
        }
        cameras.forEach((cam) => {
          const opt = document.createElement("option");
          opt.value = cam.name;
          opt.textContent = cam.name + (cam.active ? " (active)" : "");
          if (cam.active) opt.selected = true;
          cameraSelect.appendChild(opt);
        });
        /* Sync the camera_name widget. */
        const nameWidget = node.widgets?.find((w) => w.name === "camera_name");
        if (nameWidget && cameraSelect.value) {
          nameWidget.value = cameraSelect.value;
        }
      })
      .catch(() => {
        cameraSelect.innerHTML = "<option>Blender offline</option>";
      });
  }

  /* When user selects a different camera, set it active in Blender. */
  cameraSelect.addEventListener("change", () => {
    const name = cameraSelect.value;
    if (!name) return;
    const nameWidget = node.widgets?.find((w) => w.name === "camera_name");
    if (nameWidget) nameWidget.value = name;
    /* Set active in Blender. */
    fetch(getApiBase() + "/camera/active?name=" + encodeURIComponent(name)).catch(() => {});
  });

  /* --- Live preview polling --- */
  function refreshPreview() {
    if (!polling) return;
    const w = node.widgets?.find((x) => x.name === "width")?.value || 640;
    const h = node.widgets?.find((x) => x.name === "height")?.value || 480;
    const url = getApiBase() + "/preview?width=" + w + "&height=" + h + "&t=" + Date.now();

    fetch(url)
      .then((r) => {
        if (!r.ok) throw new Error(r.status);
        return r.blob();
      })
      .then((blob) => {
        const objUrl = URL.createObjectURL(blob);
        const oldSrc = img.src;
        img.src = objUrl;
        if (oldSrc && oldSrc.startsWith("blob:")) URL.revokeObjectURL(oldSrc);
        label.textContent = (cameraSelect.value || "Camera") + " - Live";
        label.style.color = "#8f8";
      })
      .catch(() => {
        label.textContent = "Camera Preview - Offline";
        label.style.color = "#f88";
      })
      .finally(() => {
        if (polling) setTimeout(refreshPreview, pollInterval);
      });
  }

  /* Init: fetch cameras, start preview. */
  setTimeout(fetchCameras, 500);
  setTimeout(refreshPreview, 1500);

  const origOnRemoved = node.onRemoved;
  node.onRemoved = function () {
    polling = false;
    if (origOnRemoved) origOnRemoved.call(this);
  };

  node.setSize([Math.max(node.size[0], 380), Math.max(node.size[1], 450)]);
}

/* ---- Register ---- */

app.registerExtension({
  name: "ComfyUI.BlenderWeb",

  setup() {
    try {
      const { ComfyButton } = window.comfyAPI.button;
      app.menu?.settingsGroup?.append(
        new ComfyButton({
          icon: "cube-outline",
          tooltip: "Open Blender Web Display",
          content: "Blender",
          action: () => window.open("http://localhost:7681", "BlenderWeb", "width=1280,height=800,resizable=yes"),
        })
      );
    } catch (e) {}
  },

  nodeCreated(node) {
    if (node.constructor?.comfyClass === "BlenderWebDisplay") {
      createBlenderWidget(node);
    }
    if (node.constructor?.comfyClass === "BlenderCameraPreview") {
      createCameraPreviewWidget(node);
    }
  },
});

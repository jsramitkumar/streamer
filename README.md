
# ESP32-CAM AI Streamer

A production-ready multi-camera streaming stack for ESP32-CAM devices.
Captures JPEG frames over UDP, runs YOLOv8 object detection, streams results
via WebSocket to a live browser dashboard, and records segmented MP4 files.

## Architecture

```
ESP32-CAM
  |
  | UDP  (JPEG frames: "<camId>|<jpeg bytes>")
  |
  v
[backend] ──HTTP──> [ai-service]  (YOLOv8 detection)
    |                   ^
    | WebSocket         | sampled frames
    v
[frontend / nginx]  (browser dashboard)

[mqtt broker]  <── MQTT ──> ESP32-CAM  (remote control)
         ^─── MQTT ──> [backend]        (status, commands)
```

## Services

| Service  | Default Port | Description                              |
|----------|-------------|------------------------------------------|
| frontend | 8080        | Nginx SPA dashboard + reverse proxy      |
| backend  | 3000 (HTTP) | REST API, WebSocket relay                |
| backend  | 5000 (UDP)  | ESP32-CAM frame ingest                   |
| ai       | internal    | YOLOv8 detection (gunicorn + Flask)      |
| mqtt     | 1883        | Eclipse Mosquitto 2.0 broker             |

## Quick Start

### 1. Clone and configure

```bash
git clone <repo>
cd streamer
cp .env.example .env
# Edit .env as needed
```

### 2. Start the stack

```bash
docker-compose up --build
```

Open **http://localhost:8080** in your browser.

### 3. Flash the ESP32-CAM

See `esp32cam/esp32cam_streamer.ino`.

**Requirements:**
- Arduino IDE 2.x
- Board: *AI Thinker ESP32-CAM* (install via Board Manager: Espressif Systems)
- Libraries (install via Library Manager):
  - `PubSubClient` by Nick O'Leary
- Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**

**Edit these constants before flashing:**

```cpp
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define BACKEND_UDP_IP   "192.168.x.x"   // Docker host IP
#define MQTT_BROKER      "192.168.x.x"   // Same host
#define CAMERA_ID        "cam1"           // Unique per device
```

To add a second camera, flash another board with `CAMERA_ID "cam2"` and
add it in the dashboard by typing `cam2` in the *Camera ID* field.

## Configuration

All backend/AI settings are controlled via environment variables (`.env`):

| Variable           | Default     | Description                                       |
|--------------------|-------------|---------------------------------------------------|
| `FRONTEND_PORT`    | `8080`      | Host port for the web dashboard                   |
| `BACKEND_HTTP_PORT`| `3000`      | Host port for backend HTTP/WS                     |
| `BACKEND_UDP_PORT` | `5000`      | Host UDP port for ESP32-CAM frames                |
| `MQTT_PORT`        | `1883`      | Host MQTT port                                    |
| `MQTT_USER`        | *(empty)*   | MQTT username (blank = anonymous)                 |
| `MQTT_PASS`        | *(empty)*   | MQTT password                                     |
| `AI_MODEL_PATH`    | `yolov8n.pt`| YOLO model (e.g. `yolov8s.pt`, `yolov8m.pt`)     |
| `AI_CONF_THRESH`   | `0.40`      | Detection confidence threshold (0.0–1.0)          |
| `AI_SAMPLE_RATE`   | `0.33`      | Fraction of frames sent to AI (0.0–1.0)           |
| `SEG_TIME`         | `300`       | Recording segment length in seconds               |
| `MAX_FRAME_BYTES`  | `5242880`   | Maximum UDP frame size (bytes)                    |
| `CORS_ORIGIN`      | `*`         | CORS origin for backend API                       |

## MQTT Remote Control

Publish to topic `streamer/<cameraId>/cmd`:

| Payload              | Effect                              |
|----------------------|-------------------------------------|
| `flash_on`           | Turn flash LED on                   |
| `flash_off`          | Turn flash LED off                  |
| `reboot`             | Restart the ESP32-CAM               |
| `stream_on`          | Resume streaming                    |
| `stream_off`         | Pause streaming                     |
| `quality:<10-63>`    | Set JPEG quality (lower = better)   |
| `interval:<33-5000>` | Set frame interval in milliseconds  |

Example using `mosquitto_pub`:
```bash
mosquitto_pub -h localhost -t streamer/cam1/cmd -m flash_on
```

## API Endpoints

```
GET  /healthz                          # Backend health probe
GET  /api/recordings/:camId            # List MP4 files for a camera
GET  /api/recordings/:camId/:file      # Stream/download a recording
WS   /ws/:camId                        # Live JPEG + detection stream
```

## Recordings

Recordings are stored in a named Docker volume `recordings` mounted at
`/app/recordings/<camId>/YYYY-MM-DD_HH-MM-SS.mp4`.

To access recordings on the host:
```bash
docker cp $(docker-compose ps -q backend):/app/recordings ./recordings
```

## GPU Acceleration (optional)

To enable CUDA inference in the AI service, change the base image in
`ai-service/Dockerfile`:

```dockerfile
FROM python:3.11-slim   →   FROM nvidia/cuda:12.4.1-runtime-ubuntu22.04
```

And add the nvidia runtime to `docker-compose.yml` under the `ai` service:
```yaml
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]
```

## Security Notes

- Change `CORS_ORIGIN` from `*` to your origin domain in production.
- Enable MQTT authentication by setting `MQTT_USER`/`MQTT_PASS` and
  updating `mosquitto/mosquitto.conf` with `allow_anonymous false` plus
  a password file.
- Place Nginx behind a TLS-terminating reverse proxy (e.g. Traefik,
  Caddy, or AWS ALB) for HTTPS/WSS in public deployments.
- The backend validates camera IDs (alphanumeric/dash/underscore only)
  and JPEG magic bytes on every UDP packet to prevent injection attacks.

---

## Deploying on Raspberry Pi 4

### Prerequisites on the RPi4

```bash
# Install Docker + Compose plugin
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker

# Optional but recommended: add a 2 GB swap file (helps during image builds)
sudo fallocate -l 2G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# Enable cgroup memory (required for Docker memory limits)
sudo sed -i 's/$/ cgroup_enable=cpuset cgroup_enable=memory cgroup_memory=1/' \
  /boot/cmdline.txt
sudo reboot
```

### Run the stack on RPi4

```bash
git clone <repo>
cd streamer
cp .env.example .env
# Edit .env – fill in CF_TUNNEL_TOKEN (see below), adjust ports if needed

docker compose -f docker-compose.yml -f docker-compose.rpi4.yml up --build -d
```

The first build downloads ~1.5 GB of images and the YOLOv8 model.  
Allow **10–20 minutes** on a fresh RPi4.

### Useful commands on RPi4

```bash
# Follow all logs
docker compose -f docker-compose.yml -f docker-compose.rpi4.yml logs -f

# Check memory usage
docker stats --no-stream

# Stop the stack
docker compose -f docker-compose.yml -f docker-compose.rpi4.yml down
```

---

## Cloudflare Tunnel (internet access)

The Cloudflare Tunnel lets you reach the dashboard, live feeds, and recordings  
from anywhere on the internet **without opening any firewall ports**.

```
Browser (internet)
  │  HTTPS / WSS
  ▼
Cloudflare Edge ──── encrypted tunnel ────► cloudflared (Docker container)
                                                │  HTTP / WS (plain, internal)
                                                ▼
                                           nginx (frontend:80)
                                           ├── /         → SPA dashboard
                                           ├── /api/*    → backend REST API
                                           └── /ws/*     → backend WebSocket
```

> **UDP (port 5000)** — the ESP32-CAM → RPi4 path — stays entirely on the  
> **local LAN**. Cloudflare Tunnel carries HTTP/WebSocket only.  
> MQTT (port 1883) is also LAN-only.

### Step-by-step Cloudflare Tunnel setup

1. **Create a free Cloudflare account** and add your domain.

2. Go to **Zero Trust** → **Networks** → **Tunnels** → **Create a tunnel**.  
   Choose **Cloudflared**, give it a name (e.g. `rpi4-streamer`), click Save.

3. Cloudflare will show a token.  Add it to your `.env`:
   ```
   CF_TUNNEL_TOKEN=eyJhIjoiYWJjZGVmZ...
   ```

4. Still in the Cloudflare dashboard, add a **Public Hostname**:

   | Field      | Value                        |
   |------------|------------------------------|
   | Subdomain  | `cam` (or anything you like) |
   | Domain     | `your-domain.com`            |
   | Service    | `HTTP`                       |
   | URL        | `frontend:80`                |

   Click **Save hostname**.

5. Start (or restart) the stack:
   ```bash
   docker compose -f docker-compose.yml -f docker-compose.rpi4.yml up -d cloudflared
   ```

6. Open `https://cam.your-domain.com` — you should see the dashboard  
   with an auto-configured TLS certificate from Cloudflare.

### Optional: lock down access with Cloudflare Access

In Zero Trust → **Access** → **Applications** you can require  
Google/GitHub login (or a one-time PIN) before anyone can open the tunnel URL.  
This is the recommended way to protect the dashboard without VPN.

---

## Performance notes for RPi4

| Service   | Typical RAM | Notes                                        |
|-----------|-------------|----------------------------------------------|
| backend   | ~80 MB      | Node.js UDP + WS                             |
| ai        | ~600 MB     | YOLOv8n on CPU; ~2–4 s per frame             |
| frontend  | ~20 MB      | Nginx static + reverse proxy                 |
| mqtt      | ~10 MB      | Mosquitto                                    |
| cloudflared| ~30 MB     | Cloudflare tunnel daemon                     |

The `docker-compose.rpi4.yml` override sets `AI_SAMPLE_RATE=0.10` by default  
(one in ten frames is analysed) to keep the RPi4 CPU responsive.  
Raise it in `.env` if you have a 8 GB RPi4 or need more frequent detections.

---

## File Structure

```
.
├── docker-compose.yml
├── docker-compose.rpi4.yml    # RPi4 + Cloudflare Tunnel override
├── .env.example
├── mosquitto/
│   └── mosquitto.conf
├── backend/
│   ├── server.js              # UDP ingest, WS relay, REST API, recorder
│   ├── package.json
│   └── Dockerfile
├── ai-service/
│   ├── app.py                 # YOLOv8 Flask/gunicorn detection service
│   ├── requirements.txt
│   └── Dockerfile
├── frontend/
│   ├── index.html             # Multi-camera SPA dashboard
│   ├── nginx.conf             # Nginx config + WS/API proxy
│   └── Dockerfile
└── esp32cam/
    └── esp32cam_streamer.ino  # Arduino sketch for AI-Thinker ESP32-CAM
```


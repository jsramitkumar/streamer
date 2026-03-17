
# ── Build stage ──────────────────────────────────────────────────────────────
# Build context is the project root (see docker-compose.yml)
FROM node:18-alpine AS builder
WORKDIR /app
COPY backend/package*.json ./
RUN npm ci --omit=dev

# ── Runtime stage ────────────────────────────────────────────────────────────
FROM node:18-alpine
WORKDIR /app

# Install FFmpeg and tini (PID-1 signal proxy)
RUN apk add --no-cache ffmpeg tini

# Non-root user
RUN addgroup -S streamer && adduser -S streamer -G streamer

COPY --from=builder /app/node_modules ./node_modules
COPY backend/server.js .

# Bundle the frontend SPA directly into the backend image
RUN mkdir -p /app/public
COPY frontend/index.html ./public/index.html

RUN mkdir -p /app/recordings && chown -R streamer:streamer /app
USER streamer

EXPOSE 3000
EXPOSE 5000/udp

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD wget -qO- http://localhost:3000/healthz || exit 1

ENTRYPOINT ["/sbin/tini", "--"]
CMD ["node", "server.js"]

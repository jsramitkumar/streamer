
FROM python:3.11-slim

WORKDIR /app

# System dependencies
# libgl1 / libglib2.0-0 – OpenCV; libgomp1 – PyTorch OpenMP (critical on ARM64)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 libglib2.0-0 libgomp1 wget ca-certificates \
  && rm -rf /var/lib/apt/lists/*

# Non-root user
RUN groupadd -r aiuser && useradd -r -g aiuser aiuser

# Install Python dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app.py .

RUN chown -R aiuser:aiuser /app
USER aiuser

EXPOSE 5001

HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
  CMD wget -qO- http://localhost:5001/healthz || exit 1

CMD ["gunicorn", "--bind", "0.0.0.0:5001", \
     "--workers", "1", \
     "--threads", "4", \
     "--timeout", "60", \
     "--access-logfile", "-", \
     "app:app"]

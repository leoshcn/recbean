# Stage 1: build the Vue frontend
FROM node:22-slim AS web
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

# Stage 2: Python runtime (FastAPI + uvicorn)
FROM python:3.12-slim
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    DATA_DIR=/data \
    WEB_DIST=/app/web/dist
WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY app/ ./app/
COPY tools/ ./tools/
COPY --from=web /web/dist ./web/dist
VOLUME ["/data"]
EXPOSE 8080
CMD ["uvicorn", "app.main:default_app", "--factory", "--host", "0.0.0.0", "--port", "8080"]

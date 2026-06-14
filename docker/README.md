# Docker build environment

Docker configuration for local release-build verification. It mirrors the key parameters of the GitHub Actions [`Release`](../.github/workflows/release.yml) workflow: `Ubuntu 20.04`, `Python 3.12`, `OpenSSL 3`, `AppImage` build, and release artifact generation.

This is not the product runtime environment and not the main development flow. Its purpose is to run the same build locally before or alongside CI.

## Build the local image
From the `docker/` directory:

```bash
sudo docker build --no-cache \
  --build-arg PYTHON_VERSION=3.12.8 \
  --build-arg OPENSSL_VERSION=3.2.3 \
  -t pcwcc-ubuntu20-build:local \
  -f Dockerfile.ubuntu20 .
```

After that, the expected flow is to run the release scripts from `scripts/` inside the container instead of assembling artifacts manually.

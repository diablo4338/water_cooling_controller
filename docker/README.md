# Docker build environment

These Docker configs are used only to verify release builds before publication.
They are not part of the normal development workflow or production runtime.

Build the local Ubuntu 20.04 image from the `docker/` directory:

```bash
sudo docker build --no-cache \
  --build-arg PYTHON_VERSION=3.12.8 \
  --build-arg OPENSSL_VERSION=3.2.3 \
  -t pcwcc-ubuntu20-build:local \
  -f Dockerfile.ubuntu20 .
```

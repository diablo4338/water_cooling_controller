# Docker release build config

Docker configuration for release builds used by the GitHub Actions [`Release`](../.github/workflows/release.yml) workflow. It defines the container environment for firmware and desktop packaging, including `Ubuntu 20.04`, `Python 3.12`, `OpenSSL 3`, `ESP-IDF`, AppImage tooling, and release artifact generation.

This is not the product runtime environment and not the main development flow. Its purpose is to provide the CI build environment and let the same release pipeline be reproduced locally when needed.

## Build the local image
From the `docker/` directory:

```bash
sudo docker build --no-cache \
  -t pcwcc-ubuntu20-build:local \
  -f Dockerfile.ubuntu20 .
```

After that, the expected flow is to run the release scripts from `scripts/` inside the container instead of assembling artifacts manually.

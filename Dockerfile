ARG PLATFORM=${BUILDPLATFORM:-linux/amd64}
ARG IMAGE=debian:bookworm

FROM --platform=${PLATFORM} ${IMAGE}

ENV LANG=C.UTF-8

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      "build-essential" "dkms" "dpkg" "debhelper" "devscripts" "dh-exec" "dh-sequence-dkms"

RUN if [ $(ls -1 "/lib/modules" | wc -l) -eq 1 ]; then \
      export KVER="$(ls -1 "/lib/modules" | head -n1)"; \
    else \
      export KVER="$(ls -1 "/lib/modules" | grep -v "$(uname -r)" | head -n1)"; \
    fi \
 && apt-get install -y "linux-headers-${KVER}"

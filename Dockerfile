FROM ubuntu:24.04@sha256:9c7f200c07e3d4a03e7249d2c16bf66a51120fb876aa2242cf31451412543289 AS engine
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build autoconf automake libtool texinfo pkgconf nasm yasm gettext ccache \
      libcurl4-openssl-dev libssl-dev libgl-dev libdbus-1-dev libfontconfig1-dev libfreetype-dev libx11-dev libudev-dev \
      libva-dev libjpeg-turbo8-dev liblzma-dev libxkbcommon-dev libwayland-dev wayland-protocols extra-cmake-modules \
      libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libegl-dev ca-certificates git python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
ENV CCACHE_DISABLE=1
COPY engine/deps engine/deps
COPY engine/cmake engine/cmake
COPY .github/scripts/build .github/scripts/build
RUN bash -c 'set -euo pipefail; source .github/scripts/build/deps-linux.sh; configure; \
      available=$(ninja -C engine/deps/build -t targets all | cut -d: -f1); targets=(); \
      for t in $(cat .github/scripts/build/deps-targets.txt); do if grep -qxF "$t" <<<"$available"; then targets+=("$t"); fi; done; \
      echo "Building: ${targets[*]}"; cmake --build engine/deps/build --config Release --target "${targets[@]}" --parallel 4'
COPY . .
RUN set -eu; arch=$(case $(uname -m) in x86_64) echo x64 ;; aarch64) echo arm64 ;; esac); \
    GITHUB_OUTPUT=/keys bash .github/scripts/build/keys.sh "linux-$arch" linux; cat /keys; . /keys; \
    git_commit_hash="$source" bash .github/scripts/build/engine.sh linux "$arch"

FROM node:26-trixie-slim@sha256:e6bedef217677b276c591795e169840edd9096dbf13f29df8ef8d1856b8820bc AS web
WORKDIR /src
COPY web/package.json web/package-lock.json web/
RUN npm ci --prefix web --no-audit --no-fund
COPY . .
RUN npm run build --prefix web

FROM golang:1.27.2-trixie@sha256:47667934874913a449b96faa7ef4777fa285d9ec29126ee80ed61c45b7cccf0e AS go

FROM ubuntu:26.10@sha256:262df65c0ec6bf8d9dcc87454f4330a05fae4b97825f45854af3b136666584c2 AS stage
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates curl git \
      binutils python3 python3-venv libstdc++6 libgcc-s1 zlib1g libva2 libva-drm2 libexpat1 libx11-6 libjpeg-turbo8 \
      libfontconfig1 liblzma5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=go /usr/local/go /usr/local/go
ENV PATH=/opt/venv/bin:/usr/local/go/bin:$PATH GOTOOLCHAIN=local
COPY engine/scripts/qualification/requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && pip install --no-cache-dir -r /tmp/requirements.txt
WORKDIR /src
COPY . .
COPY --from=engine /keys /keys
COPY --from=engine /src/build-native/tool/nonplanar_slice_tool build-native/tool/
COPY --from=engine /src/engine/deps/build/OrcaSlicer_dep/usr/local/share /deps/share
COPY --from=web /src/web/dist web/dist
ARG PREBUILT_REPO=ZimengXiong/PancakeSlicer
RUN set -eu; arch=$(case $(uname -m) in x86_64) echo x64 ;; aarch64) echo arm64 ;; esac); . /keys; mkdir /reference; \
    code=$(curl -sSL --retry 5 -o /tmp/reference.tar.gz -w '%{http_code}' \
      "https://github.com/$PREBUILT_REPO/releases/download/prebuilt/$reference"); \
    case "$code" in \
      200) tar -xzf /tmp/reference.tar.gz -C /reference ;; \
      404) [ "$arch" = x64 ] || { echo "$reference is not published in $PREBUILT_REPO; only Linux x64 can generate it" >&2; exit 1; } ;; \
      *) echo "Fetching $reference failed with HTTP $code" >&2; exit 1 ;; \
    esac; rm -f /tmp/reference.tar.gz; \
    git archive --format=tar.gz -o /tmp/source.tar.gz HEAD; \
    python3 desktop/scripts/stage-ci.py --os linux --arch "$arch" --native --tool build-native/tool/nonplanar_slice_tool \
      --reference /reference --engine-source "$source" --dependency-prefix /deps \
      --source-sha256 "$(sha256sum /tmp/source.tar.gz | cut -d' ' -f1)"; \
    mkdir /out; mv "desktop/build/engine-linux-$arch" /out/engine; mv desktop/build/web /out/web; \
    install -m 0644 /tmp/source.tar.gz /out/engine/source.tar.gz

FROM ubuntu:26.10@sha256:262df65c0ec6bf8d9dcc87454f4330a05fae4b97825f45854af3b136666584c2 AS runtime-libraries
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates ffmpeg nodejs util-linux libstdc++6 libgcc-s1 zlib1g libva2 libva-drm2 libexpat1 libx11-6 libjpeg-turbo8 libfontconfig1 liblzma5 && rm -rf /var/lib/apt/lists/*
COPY --from=stage /out/engine/nonplanar_slice_tool /tmp/tool
RUN set -eu; stdbuf_library=$(dpkg-query -L gnu-coreutils | sed -n "/\/libstdbuf\.so$/p"); test -n "$stdbuf_library"; mkdir -p /runtime/usr/bin; cp -L /usr/bin/gnustdbuf /runtime/usr/bin/stdbuf; cp -L /usr/bin/ffmpeg /runtime/usr/bin/ffmpeg; cp -L /usr/bin/node /usr/bin/prlimit /runtime/usr/bin/; cp -L --parents /usr/share/doc/ffmpeg/copyright /usr/share/doc/nodejs/copyright /usr/share/doc/util-linux/copyright /runtime; cp -L --parents "$stdbuf_library" /usr/share/doc/gnu-coreutils/copyright /runtime; ldd /tmp/tool /usr/bin/ffmpeg /usr/bin/node /usr/bin/prlimit /usr/bin/gnustdbuf "$stdbuf_library" > /tmp/dependencies; ! grep 'not found' /tmp/dependencies;  awk '/=> \// {print $3} /^[[:space:]]*\/.*ld-linux/ {print $1}' /tmp/dependencies | sort -u | while read library; do cp -L --parents "$library" /runtime; package=$(dpkg-query -S "$(readlink -f "$library")" | sed -n '1s/:.*//p'); cp -L --parents "/usr/share/doc/$package/copyright" /runtime; done
RUN mkdir -p /runtime/usr/share/nodejs \
    && cp -a /usr/share/nodejs/acorn /usr/share/nodejs/acorn-walk \
      /usr/share/nodejs/balanced-match /usr/share/nodejs/brace-expansion /usr/share/nodejs/llhttp \
      /usr/share/nodejs/minimatch /usr/share/nodejs/undici /usr/share/nodejs/undici-types /runtime/usr/share/nodejs/ \
    && cp -L --parents /usr/share/doc/node-acorn/copyright /usr/share/doc/node-balanced-match/copyright \
      /usr/share/doc/node-brace-expansion/copyright /usr/share/doc/node-llhttp/copyright \
      /usr/share/doc/node-minimatch/copyright /usr/share/doc/node-undici/copyright /runtime
RUN node -e 'const [major, minor] = process.versions.node.split(".").map(Number); if (major < 20 || major === 20 && minor < 10) process.exit(1)'
RUN mkdir -p /runtime/tmp /runtime/data /runtime/etc/ssl/certs && chmod 1777 /runtime/tmp && chown 65532:65532 /runtime/data && cp /etc/ssl/certs/ca-certificates.crt /runtime/etc/ssl/certs/ && cp -r /etc/fonts /runtime/etc/ && printf 'nonroot:x:65532:65532:nonroot:/data:/sbin/nologin\n' > /runtime/etc/passwd && printf 'nonroot:x:65532:\n' > /runtime/etc/group && printf 'hosts: files dns\n' > /runtime/etc/nsswitch.conf
FROM scratch
ARG VERSION=source
LABEL org.opencontainers.image.title="PancakeSlicer" org.opencontainers.image.description="PancakeSlicer web UI, service and non-planar slicing engine" org.opencontainers.image.licenses="AGPL-3.0-only" org.opencontainers.image.version="${VERSION}"
COPY --from=runtime-libraries /runtime/ /
COPY --from=stage /out/engine/ /app/engine/
COPY --from=stage /out/web/ /app/web/
USER 65532:65532
WORKDIR /app
ENV PATH=/usr/bin:/bin HOME=/data TZ=UTC ORCA_WEB_LISTEN=0.0.0.0:6868 ORCA_WEB_DATA_DIR=/data/library ORCA_WEB_JOB_ROOT=/data/jobs ORCA_WEB_STATIC=/app/web ORCA_WEB_TOOL=/app/engine/nonplanar_slice_tool ORCA_WEB_SUPPORT_FINS_NODE=/usr/bin/node ORCA_WEB_SUPPORT_FINS_CLI=/app/engine/support-fins/support-fins.mjs ORCA_WEB_RESOURCES=/app/engine/resources ORCA_WEB_SURFACE_MANIFEST=/app/engine/surface-manifest.json ORCA_WEB_ARTIFACT_QUALIFICATION=/app/engine/artifact-qualification.ready ORCA_WEB_SOURCE_ARCHIVE=/app/engine/source.tar.gz ORCA_WEB_SOURCE_URL= ORCA_BUILD_IMAGE=pancakeslicer:${VERSION}
EXPOSE 6868
VOLUME ["/data"]
ENTRYPOINT ["/app/engine/orca-webapp-server"]

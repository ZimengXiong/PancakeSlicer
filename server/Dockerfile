FROM ubuntu:26.10@sha256:262df65c0ec6bf8d9dcc87454f4330a05fae4b97825f45854af3b136666584c2 AS runtime-libraries
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates ffmpeg libstdc++6 libgcc-s1 zlib1g libva2 libva-drm2 libexpat1 libx11-6 libjpeg-turbo8 libfontconfig1 liblzma5 && rm -rf /var/lib/apt/lists/*
ARG ENGINE
COPY ${ENGINE}/nonplanar_slice_tool /tmp/tool
RUN set -eu; stdbuf_library=$(dpkg-query -L gnu-coreutils | sed -n "/\/libstdbuf\.so$/p"); test -n "$stdbuf_library"; mkdir -p /runtime/usr/bin; cp -L /usr/bin/gnustdbuf /runtime/usr/bin/stdbuf; cp -L /usr/bin/ffmpeg /runtime/usr/bin/ffmpeg; cp -L --parents /usr/share/doc/ffmpeg/copyright /runtime; cp -L --parents "$stdbuf_library" /usr/share/doc/gnu-coreutils/copyright /runtime; ldd /tmp/tool /usr/bin/ffmpeg /usr/bin/gnustdbuf "$stdbuf_library" > /tmp/dependencies; ! grep 'not found' /tmp/dependencies;  awk '/=> \// {print $3} /^[[:space:]]*\/.*ld-linux/ {print $1}' /tmp/dependencies | sort -u | while read library; do cp -L --parents "$library" /runtime; package=$(dpkg-query -S "$(readlink -f "$library")" | sed -n '1s/:.*//p'); cp -L --parents "/usr/share/doc/$package/copyright" /runtime; done
RUN mkdir -p /runtime/tmp /runtime/data /runtime/etc/ssl/certs && chmod 1777 /runtime/tmp && chown 65532:65532 /runtime/data && cp /etc/ssl/certs/ca-certificates.crt /runtime/etc/ssl/certs/ && cp -r /etc/fonts /runtime/etc/ && printf 'nonroot:x:65532:65532:nonroot:/data:/sbin/nologin\n' > /runtime/etc/passwd && printf 'nonroot:x:65532:\n' > /runtime/etc/group && printf 'hosts: files dns\n' > /runtime/etc/nsswitch.conf
FROM scratch
ARG ENGINE
COPY --from=runtime-libraries /runtime/ /
COPY ${ENGINE}/ /app/engine/
COPY web/ /app/web/
USER 65532:65532
WORKDIR /app
ENV PATH=/usr/bin:/bin HOME=/data TZ=UTC ORCA_WEB_LISTEN=0.0.0.0:8780 ORCA_WEB_DATA_DIR=/data/library ORCA_WEB_JOB_ROOT=/data/jobs ORCA_WEB_STATIC=/app/web ORCA_WEB_TOOL=/app/engine/nonplanar_slice_tool ORCA_WEB_RESOURCES=/app/engine/resources ORCA_WEB_SURFACE_MANIFEST=/app/engine/surface-manifest.json ORCA_WEB_ARTIFACT_QUALIFICATION=/app/engine/artifact-qualification.ready ORCA_WEB_SOURCE_URL=https://github.com/ZimengXiong/PancakeSlicer/archive/{revision}.tar.gz
EXPOSE 8780
VOLUME ["/data"]
ENTRYPOINT ["/app/engine/orca-webapp-server"]

FROM 127.0.0.1:5001/neon/pg17-native-root-wire@sha256:af97e387041b349d0138b39b9a7c90c9d612db3ef07906fe1271db7162ee414d
WORKDIR /home/nonroot
COPY --chown=1000:1000 dsg_envelope_filter.c vendor/postgres-v17/src/backend/storage/buffer/dsg_envelope_filter.c
RUN --mount=type=cache,id=neon-ccache-amd64,target=/home/nonroot/.cache/ccache,uid=1000,gid=1000,sharing=shared sha256sum vendor/postgres-v17/src/backend/storage/buffer/dsg_envelope_filter.c && mold -run make -C build/v17 -j3 -s && make -C build/v17 -s install
WORKDIR /workspace/neon

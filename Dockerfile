# Fast build: use Chimera's system LLVM, only compile the plugin
FROM docker.io/chimeralinux/chimera:latest

RUN apk update && apk add \
    cmake ninja ccache clang clang-devel llvm-devel lld-devel lld \
    musl-devel linux-headers \
    && rm -rf /var/cache/apk/*

COPY perfsanitizer /src/perfsanitizer

RUN cmake -G Ninja -S /src/perfsanitizer -B /build \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DLLVM_DIR=$(llvm-config --cmakedir) \
    -DClang_DIR=$(dirname $(llvm-config --cmakedir))/clang \
    && cmake --build /build -- -j$(nproc)

ENV PLUGIN=/build/libPerfSanitizer.so

RUN echo "=== PerfSanitizer Demo ===" && \
    clang++ -fplugin=$PLUGIN -O2 -c /src/perfsanitizer/tools/demo.cpp -o /dev/null 2>&1 || true

WORKDIR /work
CMD ["/bin/sh"]

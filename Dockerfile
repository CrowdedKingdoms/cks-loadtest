# cks-loadtest — UDP load tester for Crowded Kingdoms game servers.

FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake libcurl4-openssl-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
COPY third_party ./third_party
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home loadtest
COPY --from=build /src/build/cks-loadtest /usr/local/bin/cks-loadtest
USER loadtest
ENTRYPOINT ["cks-loadtest"]

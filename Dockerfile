# syntax=docker/dockerfile:1

# Build stage
FROM debian:testing AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies directly
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-15 \
    g++-15 \
    git \
    cmake \
    ninja-build \
    pkg-config \
    libpq-dev \
    libxxhash-dev \
    libpoppler-glib-dev \
    libglib2.0-dev \
    libcairo2-dev \
    zlib1g-dev \
    ca-certificates \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Set GCC 15 as default compiler
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-15 100 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-15 100 && \
    update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-15 100 && \
    gcc --version

WORKDIR /build

# Clone and build dependencies
# solidc
RUN git clone --depth 1 https://github.com/abiiranathan/solidc.git && \
    cd solidc && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=/usr/bin/gcc-15 \
          -DCMAKE_CXX_COMPILER=/usr/bin/g++-15 && \
    cmake --build build && \
    cmake --install build && \
    cd .. && rm -rf solidc

# pgconn
RUN git clone --depth 1 https://github.com/abiiranathan/pgconn.git && \
    cd pgconn && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=/usr/bin/gcc-15 \
          -DCMAKE_CXX_COMPILER=/usr/bin/g++-15 && \
    cmake --build build && \
    cmake --install build && \
    cd .. && rm -rf pgconn

# pulsar
RUN git clone --depth 1 https://github.com/abiiranathan/pulsar.git && \
    cd pulsar && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=/usr/bin/gcc-15 \
          -DCMAKE_CXX_COMPILER=/usr/bin/g++-15 && \
    cmake --build build && \
    cmake --install build && \
    cd .. && rm -rf pulsar

# yyjson
RUN git clone --depth 1 https://github.com/ibireme/yyjson.git && \
    cd yyjson && \
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=/usr/bin/gcc-15 \
          -DCMAKE_CXX_COMPILER=/usr/bin/g++-15 && \
    cmake --build build && \
    cmake --install build && \
    cd .. && rm -rf yyjson

# Copy source code
COPY . /build/lexicon

# Build lexicon
WORKDIR /build/lexicon
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=/usr/bin/gcc-15 \
          -DCMAKE_CXX_COMPILER=/usr/bin/g++-15 && \
    cmake --build build -- -j$(nproc) && \
    strip build/lexicon

# Runtime stage
FROM debian:testing-slim

# Install runtime dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    libpoppler-glib8 \
    libglib2.0-0 \
    libcairo2 \
    zlib1g \
    ca-certificates \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -r -u 1000 -m -s /bin/bash lexicon

WORKDIR /app

# Copy built binary and UI from builder
COPY --from=builder /build/lexicon/build/lexicon /app/
COPY --from=builder /build/lexicon/ui/dist /app/ui/dist

# Copy shared libraries installed by cmake
COPY --from=builder /usr/local/lib/*.so* /usr/local/lib/
RUN ldconfig

# Create mount point for PDFs
RUN mkdir -p /pdfs && chown lexicon:lexicon /pdfs

# Switch to non-root user
USER lexicon

# Expose default port
EXPOSE 8080

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD ["/bin/sh", "-c", "wget --no-verbose --tries=1 --spider http://localhost:8080/ || exit 1"]

# Default command - server mode
ENTRYPOINT ["/app/lexicon"]
CMD ["--port", "8080", "--addr", "0.0.0.0"]

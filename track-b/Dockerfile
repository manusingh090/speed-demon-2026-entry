# Build with the standard Rust toolchain, tuned for the scoring CPU, then ship the binary alone.
FROM rust:1-slim-trixie@sha256:1a939748a7cbab172de91a014e3804a97af1394a8c844faa67fce9401c4e40fb AS build
WORKDIR /src
COPY Cargo.toml .
COPY src src
RUN RUSTFLAGS="-C target-cpu=native" cargo build --release --offline

FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /src/target/release/index /app/index
ENTRYPOINT ["/app/index"]

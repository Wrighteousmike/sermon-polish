#!/bin/sh
# Builds DeepFilterNet's static C library (libdf.a) into
# third_party/DeepFilterNet/target/release. Needs a Rust toolchain; the
# project keeps it on the external drive:
#   export RUSTUP_HOME="$HOME/toolchains/rustup"
#   export CARGO_HOME="$HOME/toolchains/cargo"
#   export PATH="$CARGO_HOME/bin:$PATH"
# (install with rustup-init --no-modify-path --profile minimal; see docs.)
set -e
# Windows (MSYS2): the Rust toolchain lives under the Windows profile, which the MSYS shell does not search.
if ! command -v cargo >/dev/null 2>&1 && [ -n "$CARGO_HOME" ]; then PATH="$(cygpath -u "$CARGO_HOME" 2>/dev/null || echo "$CARGO_HOME")/bin:$PATH"; fi
[ -z "$RUSTUP_HOME" ] && [ -n "$USERPROFILE" ] && export RUSTUP_HOME="$USERPROFILE/.rustup"
cd "$(dirname "$0")/../third_party"
[ -d DeepFilterNet ] || git clone --depth 1 https://github.com/Rikorose/DeepFilterNet.git DeepFilterNet
cd DeepFilterNet
# Keep the deployment target in step with CMakeLists.txt (macOS) so the library runs on older systems.
export MACOSX_DEPLOYMENT_TARGET=11.0
# ARCH=x86_64 builds the Intel library on an Apple silicon Mac (rustup target add x86_64-apple-darwin first);
# RUST_TARGET=x86_64-pc-windows-gnu builds the Windows (MinGW) one. Output: target/<triple>/release.
[ "$ARCH" = "x86_64" ] && [ "$(uname -m)" != "x86_64" ] && [ -z "$RUST_TARGET" ] && RUST_TARGET=x86_64-apple-darwin
TARGET_ARGS=""
[ -n "$RUST_TARGET" ] && TARGET_ARGS="--target $RUST_TARGET"
cargo rustc --release $TARGET_ARGS -p deep_filter --lib --no-default-features --features capi --crate-type staticlib

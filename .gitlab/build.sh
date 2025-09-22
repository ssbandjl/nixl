#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e
set -x
set -o pipefail

# Parse commandline arguments with first argument being the install directory
# and second argument being the UCX installation directory.
INSTALL_DIR=$1
UCX_INSTALL_DIR=$2
EXTRA_BUILD_ARGS=${3:-""}
# UCX_VERSION is the version of UCX to build override default with env variable.
UCX_VERSION=${UCX_VERSION:-v1.19.0}
# EFA_INSTALLER_VERSION is the version of EFA installer to use, defaults to "latest"
EFA_INSTALLER_VERSION=${EFA_INSTALLER_VERSION:-latest}

if [ -z "$INSTALL_DIR" ]; then
    echo "Usage: $0 <install_dir> <ucx_install_dir>"
    exit 1
fi

if [ -z "$UCX_INSTALL_DIR" ]; then
    UCX_INSTALL_DIR=$INSTALL_DIR
fi


# For running as user - check if running as root, if not set sudo variable
if [ "$(id -u)" -ne 0 ]; then
    SUDO=sudo
else
    SUDO=""
fi

ARCH=$(uname -m)
[ "$ARCH" = "arm64" ] && ARCH="aarch64"

# Some docker images are with broken installations:
$SUDO rm -rf /usr/lib/cmake/grpc /usr/lib/cmake/protobuf

$SUDO apt-get -qq update
$SUDO apt-get -qq install -y curl \
                             wget \
                             libnuma-dev \
                             numactl \
                             autotools-dev \
                             automake \
                             git \
                             libtool \
                             libz-dev \
                             libiberty-dev \
                             flex \
                             build-essential \
                             cmake \
                             libibverbs-dev \
                             libgoogle-glog-dev \
                             libgtest-dev \
                             libgmock-dev \
                             libjsoncpp-dev \
                             libpython3-dev \
                             libboost-all-dev \
                             libssl-dev \
                             libgrpc-dev \
                             libgrpc++-dev \
                             libprotobuf-dev \
                             libcpprest-dev \
                             libaio-dev \
                             liburing-dev \
                             meson \
                             ninja-build \
                             pkg-config \
                             protobuf-compiler-grpc \
                             pybind11-dev \
                             etcd-server \
                             net-tools \
                             iproute2 \
                             pciutils \
                             libpci-dev \
                             uuid-dev \
                             ibverbs-utils \
                             libibmad-dev \
                             doxygen \
                             clang \
                             hwloc \
                             libhwloc-dev \
                             libcurl4-openssl-dev zlib1g-dev # aws-sdk-cpp dependencies

wget --tries=3 --waitretry=5 https://static.rust-lang.org/rustup/dist/${ARCH}-unknown-linux-gnu/rustup-init
chmod +x rustup-init
./rustup-init -y --default-toolchain 1.86.0
export PATH="$HOME/.cargo/bin:$PATH"

curl -fSsL "https://github.com/openucx/ucx/tarball/${UCX_VERSION}" | tar xz
( \
  cd openucx-ucx* && \
  ./autogen.sh && \
  ./configure \
          --prefix="${UCX_INSTALL_DIR}" \
          --enable-shared \
          --disable-static \
          --disable-doxygen-doc \
          --enable-optimizations \
          --enable-cma \
          --enable-devel-headers \
          --with-verbs \
          --with-dm \
          --enable-mt && \
        make -j && \
        make -j install-strip && \
        $SUDO ldconfig \
)

curl -fsSL "https://efa-installer.amazonaws.com/aws-efa-installer-${EFA_INSTALLER_VERSION}.tar.gz" | tar xz
( \
  cd aws-efa-installer && \
  $SUDO ./efa_installer.sh -y -g --skip-kmod --skip-limit-conf --no-verify && \
  $SUDO ldconfig \
)

( \
  cd /tmp && \
  git clone --depth 1 https://github.com/etcd-cpp-apiv3/etcd-cpp-apiv3.git && \
  cd etcd-cpp-apiv3 && \
  mkdir build && cd build && \
  cmake .. && \
  make -j"${NPROC:-$(nproc)}" && \
  $SUDO make install && \
  $SUDO ldconfig \
)

( \
  cd /tmp && \
  git clone --recurse-submodules --depth 1 --shallow-submodules https://github.com/aws/aws-sdk-cpp.git --branch 1.11.581 && \
  mkdir aws_sdk_build && \
  cd aws_sdk_build && \
  cmake ../aws-sdk-cpp/ -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY="s3" -DENABLE_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr/local && \
  make -j"${NPROC:-$(nproc)}" && \
  $SUDO make install
)

export LIBRARY_PATH="$LIBRARY_PATH:/usr/local/cuda/lib64"
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${INSTALL_DIR}/lib/$ARCH-linux-gnu:${INSTALL_DIR}/lib64:$LD_LIBRARY_PATH:/usr/local/cuda/lib64:/usr/local/cuda/lib64/stubs:${INSTALL_DIR}/lib:/opt/amazon/efa/lib"
export CPATH="${INSTALL_DIR}/include:/opt/amazon/efa/include:$CPATH"
export PATH="${INSTALL_DIR}/bin:$PATH"
export PKG_CONFIG_PATH="${INSTALL_DIR}/lib/pkgconfig:${INSTALL_DIR}/lib64/pkgconfig:${INSTALL_DIR}:/opt/amazon/efa/lib/pkgconfig:$PKG_CONFIG_PATH"
export NIXL_PLUGIN_DIR="${INSTALL_DIR}/lib/$ARCH-linux-gnu/plugins"
export CMAKE_PREFIX_PATH="${INSTALL_DIR}:${CMAKE_PREFIX_PATH}"

# Disabling CUDA IPC not to use NVLINK, as it slows down local
# UCX transfers and can cause contention with local collectives.
export UCX_TLS=^cuda_ipc

# shellcheck disable=SC2086
meson setup nixl_build --prefix=${INSTALL_DIR} -Ducx_path=${UCX_INSTALL_DIR} -Dbuild_docs=true -Drust=false ${EXTRA_BUILD_ARGS} -Dlibfabric_path="/opt/amazon/efa"
ninja -C nixl_build && ninja -C nixl_build install

# TODO(kapila): Copy the nixl.pc file to the install directory if needed.
# cp ${BUILD_DIR}/nixl.pc ${INSTALL_DIR}/lib/pkgconfig/nixl.pc

cd benchmark/nixlbench
meson setup nixlbench_build -Dnixl_path=${INSTALL_DIR} -Dprefix=${INSTALL_DIR}
ninja -C nixlbench_build && ninja -C nixlbench_build install

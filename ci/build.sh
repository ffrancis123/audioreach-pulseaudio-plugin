#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
set -ex
echo "Running build script..."

PREBUILD_SCRIPT_PATH="${PREBUILD_SCRIPT:-$(dirname "${BASH_SOURCE[0]}")/pre_build.sh}"
source "$PREBUILD_SCRIPT_PATH"

# load build args from file if environment variable is not set
if [ -z "${BUILD_ARGS}" ]; then
    BUILD_OPTIONS_FILE="${GITHUB_WORKSPACE}/ci/build_options.txt"
    BUILD_ARGS="$(sed -E 's/#.*$//' "$BUILD_OPTIONS_FILE" | sed '/^[[:space:]]*$/d' | tr '\n' ' ')"
fi

# Build/Compile audioreach-pulseaudio-plugin
source ${GITHUB_WORKSPACE}/install/environment-setup-armv8-2a-poky-linux

# make sure we are in the right directory
cd ${GITHUB_WORKSPACE}

#install pal-headers
cd ../audioreach-pal/inc
autoreconf -Wcross --verbose --install --force --exclude=autopoint
autoconf --force
./configure ${BUILD_ARGS}
make DESTDIR=${SDKTARGETSYSROOT}  install
cd ${GITHUB_WORKSPACE}

#install libatomic
cd ../libatomic_ops
autoreconf -Wcross --verbose --install --force --exclude=autopoint
autoconf --force
./configure ${BUILD_ARGS}
make DESTDIR=${SDKTARGETSYSROOT} install
cp -r ${OECORE_NATIVE_SYSROOT}/usr/share/libtool/* ${SDKTARGETSYSROOT}/usr/include/

#compile pulseaudio-plugin
cd ${GITHUB_WORKSPACE}
# Run autoreconf to generate the configure script

#source ./install/environment-setup-armv8-2a-poky-linux
cd ./modules/pa-pal-plugins/
autoreconf -Wcross --verbose --install --force --exclude=autopoint
autoconf --force
# Run the configure script with the specified arguments
./configure ${BUILD_ARGS} --with-pa_version=17.0 --without-pa-support-card-status
# make
make DESTDIR=${GITHUB_WORKSPACE}/build install

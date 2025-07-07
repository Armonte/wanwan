#!/bin/bash
set -e
set -x

cd build
ninja || {
    echo "Build failed!"
    exit 1
}

echo "Build completed successfully!"

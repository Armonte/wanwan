#!/bin/bash
set -e
set -x
clear
cd build
ninja || {
    echo "Build failed!"
    exit 1
}

cp FM2K_RollbackLauncher.exe FM2KHook.dll /mnt/c/games/

echo "Build completed successfully!"

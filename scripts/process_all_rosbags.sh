#!/bin/bash

ROSBAG_PATH="/home/admin/rosbags/rosbags/rosbags"
PCD_SAVE_DIR="/home/admin/rosbags/rosbags/rosbags/pcd"
LOG_PATH="/home/admin/rosbags/rosbags/rosbags/pcd/loam.log"

# Function to kill all child processes upon exit
cleanup() {
    echo "Caught SIGINT, killing all child processes..."
    kill -- -$$
}

# Set trap to catch Ctrl-C and call cleanup function
trap cleanup SIGINT

source /home/admin/workspace/devel/setup.bash

for ROSBAG_FILE in "${ROSBAG_PATH}"/atc*.bag; do
    ROSBAG_FILE_NAME=$(basename "${ROSBAG_FILE}")
    ROSBAG_FILE_BASE="${ROSBAG_FILE_NAME%.bag}"  # Ensure .bag is removed from base name
        
    # Check if PCD file already exists
    if [[ -f "${PCD_SAVE_DIR}/${ROSBAG_FILE_BASE}.pcd" ]]; then
        echo "PCD file for ${ROSBAG_FILE_NAME} already exists. Skipping..."
        continue  # Skip to the next iteration of the loop
    fi

    echo "Processing ${ROSBAG_FILE_NAME}"
    roslaunch loam_horizon loam_livox_horizon.launch \
    rosbag_dir:="${ROSBAG_PATH}" \
    rosbag_name:="${ROSBAG_FILE_NAME}" \
    pcd_save_path:="${PCD_SAVE_DIR}/${ROSBAG_FILE_BASE}.pcd" >> "${LOG_PATH}" 2>&1
done

# Clean exit point
cleanup

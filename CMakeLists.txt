cmake_minimum_required(VERSION 2.8.3)
project(loam_horizon)
set(CMAKE_CXX_STANDARD 14)  # This is enough to set C++14
set(CMAKE_BUILD_TYPE "Release")

# It's better to set compiler-specific flags in a more conditional way
if(CMAKE_COMPILER_IS_GNUCXX)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_definitions(-DROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/\")

find_package(catkin REQUIRED COMPONENTS
  geometry_msgs
  nav_msgs
  sensor_msgs
  roscpp
  rospy
  rosbag
  std_msgs
  image_transport
  cv_bridge
  tf
  livox_ros_driver
)

find_package(Eigen3 REQUIRED)
find_package(PCL REQUIRED)
find_package(OpenCV 4.2.0 REQUIRED)
find_package(Ceres REQUIRED)
find_package(libLAS)  # Add this line to find libLAS
find_package(LASzip)


include_directories(
  include
  ${catkin_INCLUDE_DIRS}
  ${PCL_INCLUDE_DIRS}
  ${CERES_INCLUDE_DIRS}
  ${OpenCV_INCLUDE_DIRS}
  ${libLAS_INCLUDE_DIRS}
  ${LASZIP_INCLUDE_DIRS}
  # /lib/x86_64-linux-gnu
  # /usr/local/include/liblas  # Add this to include directories
)

catkin_package(
  CATKIN_DEPENDS geometry_msgs nav_msgs roscpp rospy std_msgs livox_ros_driver
  DEPENDS EIGEN3 PCL 
  INCLUDE_DIRS include
)

add_executable(scanRegistration src/scanRegistration.cpp)
target_link_libraries(scanRegistration ${catkin_LIBRARIES} ${PCL_LIBRARIES})

add_executable(laserOdometry src/laserOdometry.cpp)
target_link_libraries(laserOdometry ${catkin_LIBRARIES} ${PCL_LIBRARIES} ${CERES_LIBRARIES})

add_executable(laserMapping src/laserMapping.cpp)
target_link_libraries(laserMapping ${catkin_LIBRARIES} ${PCL_LIBRARIES} ${CERES_LIBRARIES} ${libLAS_LIBRARIES} laszip )

add_executable(livox_repub src/livox_repub.cpp)
target_link_libraries(livox_repub ${catkin_LIBRARIES} ${PCL_LIBRARIES} ${OpenCV_LIBS})

add_executable(imu_process src/imu_processor/data_process_node.cpp src/imu_processor/data_process.cpp
                           src/imu_processor/gyr_int.cpp)
target_link_libraries(imu_process ${catkin_LIBRARIES} ${PCL_LIBRARIES} ${OpenCV_LIBS} ${libLAS_LIBRARIES})  # Link libLAS here

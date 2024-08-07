// This is an advanced implementation of the algorithm described in the
// following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// Modifier: Livox               Livox@gmail.com

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <ceres/ceres.h>
#include <geometry_msgs/PoseStamped.h>
#include <loam_horizon/common.h>
#include <condition_variable>
#include <stdexcept>
// #include <liblas/capi/las_config.h>
#include <laszip/laszip_api.h>
#include <math.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/common.h>
#include <pcl_conversions/pcl_conversions.h>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>
#include <eigen3/Eigen/Dense>
#include <Eigen/Core>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>


#include <opencv2/opencv.hpp>
#include <cv_bridge/cv_bridge.h>


#include "lidarFactor.hpp"
#include "loam_horizon/common.h"
#include "loam_horizon/tic_toc.h"

int frameCount = 0;

double timeLaserCloudCornerLast = 0;
double timeLaserCloudSurfLast = 0;
double timeLaserCloudFullRes = 0;
double timeLaserOdometry = 0;

int laserCloudCenWidth = 10;
int laserCloudCenHeight = 10;
int laserCloudCenDepth = 5;
const int laserCloudWidth = 21;
const int laserCloudHeight = 21;
const int laserCloudDepth = 11;

const int laserCloudNum =
    laserCloudWidth * laserCloudHeight * laserCloudDepth;  // 4851

int laserCloudValidInd[125];
int laserCloudSurroundInd[125];

// input: from odom
pcl::PointCloud<PointType>::Ptr laserCloudCornerLast(
    new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr laserCloudSurfLast(
    new pcl::PointCloud<PointType>());

// ouput: all visualble cube points
pcl::PointCloud<PointType>::Ptr laserCloudSurround(
    new pcl::PointCloud<PointType>());

// surround points in map to build tree
pcl::PointCloud<PointType>::Ptr laserCloudCornerFromMap(
    new pcl::PointCloud<PointType>());
pcl::PointCloud<PointType>::Ptr laserCloudSurfFromMap(
    new pcl::PointCloud<PointType>());

// input & output: points in one frame. local --> global
pcl::PointCloud<PointType>::Ptr laserCloudFullRes(
    new pcl::PointCloud<PointType>());
pcl::PointCloud<pcl::PointXYZRGB>::Ptr laserCloudFullResColor(
    new pcl::PointCloud<pcl::PointXYZRGB>());
pcl::PointCloud<PointType>::Ptr laserCloudFullResIntensity(
    new pcl::PointCloud<PointType>());
pcl::PointCloud<pcl::PointXYZRGB>::Ptr laserColorFullRes(
  new pcl::PointCloud<pcl::PointXYZRGB>());
pcl::PointCloud<PointType>::Ptr laserCloudWaitSave(
    new pcl::PointCloud<PointType>());
pcl::PointCloud<pcl::PointXYZRGB>::Ptr laserColorCloudWaitSave(
    new pcl::PointCloud<pcl::PointXYZRGB>());


// points in every cube
pcl::PointCloud<PointType>::Ptr laserCloudCornerArray[laserCloudNum];
pcl::PointCloud<PointType>::Ptr laserCloudSurfArray[laserCloudNum];

// kd-tree
pcl::KdTreeFLANN<PointType>::Ptr kdtreeCornerFromMap(
    new pcl::KdTreeFLANN<PointType>());
pcl::KdTreeFLANN<PointType>::Ptr kdtreeSurfFromMap(
    new pcl::KdTreeFLANN<PointType>());

double parameters[7] = {0, 0, 0, 1, 0, 0, 0};
Eigen::Map<Eigen::Quaterniond> q_w_curr(parameters);
Eigen::Map<Eigen::Vector3d> t_w_curr(parameters + 4);

// wmap_T_odom * odom_T_curr = wmap_T_curr;
// transformation between odom's world and map's world frame
Eigen::Quaterniond q_wmap_wodom(1, 0, 0, 0);
Eigen::Vector3d t_wmap_wodom(0, 0, 0);

Eigen::Quaterniond q_wodom_curr(1, 0, 0, 0);
Eigen::Vector3d t_wodom_curr(0, 0, 0);

std::queue<sensor_msgs::PointCloud2ConstPtr> cornerLastBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> surfLastBuf;
std::queue<sensor_msgs::PointCloud2ConstPtr> fullResBuf;
std::queue<nav_msgs::Odometry::ConstPtr> odometryBuf;
std::mutex mBuf;
std::mutex mOdom;
std::mutex mCam;


pcl::VoxelGrid<PointType> downSizeFilterCorner;
pcl::VoxelGrid<PointType> downSizeFilterSurf;

std::vector<int> pointSearchInd;
std::vector<float> pointSearchSqDis;

PointType pointOri, pointSel;

ros::Publisher pubLaserCloudSurround, pubLaserCloudMap, pubLaserCloudFullRes,
    pubOdomAftMapped, pubOdomAftMappedHighFrec, pubLaserAfterMappedPath, pubLaserColor;

nav_msgs::Path laserAfterMappedPath;

vector<double>       extrinT(3, 0.0);
vector<double>       extrinR(9, 0.0);
// color mapping param
vector<double>       extrinT_lc(3, 0.0);
vector<double>       extrinR_lc(9, 0.0);
vector<double>       K_camera(9, 0.0);
vector<double>       D_camera(5, 0.0);
bool   camera_pushed = false;
deque<sensor_msgs::ImagePtr>      camera_buffer;
deque<double>                     camera_time_buffer;
std::condition_variable sig_cam_buffer;
Eigen::Vector3d Lidar_T_wrt_IMU(Eigen::Vector3d(0,0,0));
Eigen::Matrix3d Lidar_R_wrt_IMU(Eigen::Matrix3d::Identity());
Eigen::Vector3d Camera_T_wrt_Lidar(Eigen::Vector3d(0,0,0));
Eigen::Matrix3d Camera_R_wrt_Lidar(Eigen::Matrix3d::Identity());

#define VEC_FROM_ARRAY(v)        v[0],v[1],v[2]
#define MAT_FROM_ARRAY(v)        v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8]
bool use_color;


bool find_best_camera_match(double lidar_time, int& best_id)
{


    if (camera_time_buffer.empty())
        return false;
    int left = 0;
    int right = camera_time_buffer.size() - 1;
    int middle = 0;
    while (left < right)
    {
        middle = left + (right - left) / 2;
        if (camera_time_buffer[middle] > lidar_time)
        {
            right = middle;
        }
        else if (camera_time_buffer[middle] < lidar_time)
        {
            left = middle + 1;
        }
        else
        {
            best_id = middle;
            return true;
        }
    }
    best_id = middle;
    double max_time_diff = 100;
    ROS_WARN("lidar_time and best id: %lf, %d", lidar_time, best_id);
    ROS_WARN("lidar_time - camera_time_buffer[best_id]: %lf", lidar_time - camera_time_buffer[best_id]);
    if (lidar_time - camera_time_buffer[best_id] > max_time_diff)
        return false;
    else
        return true;
}

Eigen::Vector2d distort(Eigen::Vector2d point)
{
    double k1 = D_camera[0];
    double k2 = D_camera[1];
    double k3 = D_camera[4];
    double p1 = D_camera[2];
    double p2 = D_camera[3];

    double x2 = point.x() * point.x();
    double y2 = point.y() * point.y();

    double r2 = x2 + y2;
    double r4 = r2 * r2;
    double r6 = r2 * r4;

    double r_coeff = 1.0 + k1 * r2 + k2 * r4 + k3 * r6;
    double t_coeff1 = 2.0 * point.x() * point.y();
    double t_coeff2 = r2 + 2.0 * x2;
    double t_coeff3 = r2 + 2.0 * y2;
    double x = r_coeff * point.x() + p1 * t_coeff1 + p2 * t_coeff2;
    double y = r_coeff * point.y() + p1 * t_coeff3 + p2 * t_coeff1;
    return Eigen::Vector2d(x, y);

}



void CameraRGBAssociateToMap(PointType const *const pi, pcl::PointXYZRGB *const po, cv::Mat& rgb){

  Eigen::Vector3d point_pc = {pi->x, pi->y, pi->z};
  Eigen::Vector3d point_camera = Camera_R_wrt_Lidar * point_pc + Camera_T_wrt_Lidar;
  Eigen::Vector3d point_w = q_w_curr * point_pc + t_w_curr;
  if (point_camera.z() > 0)
  {
      Eigen::Vector2d point_2d = (point_camera.head<2>() / point_camera.z()).eval();
      Eigen::Vector2d point_2d_dis = distort(point_2d);
      int u = static_cast<int>(K_camera[0] * point_2d_dis.x() + K_camera[2]);
      int v = static_cast<int>(K_camera[4] * point_2d_dis.y() + K_camera[5]);

      // vu.push_back({v,u});
      // ROS_WARN("point_2d: %lf %lf", point_2d.x(), point_2d.y());
      // ROS_WARN("point_2d_dis: %lf %lf", point_2d_dis.x(), point_2d_dis.y());
      // ROS_WARN("u: %d, v: %d", u, v);
      if (u >= 0 && u < rgb.cols && v >= 0 && v < rgb.rows)
      {
          // pcl::PointXYZRGB point_rgb;
          po->x = point_w.x();
          po->y = point_w.y();
          po->z = point_w.z();
          po->b = (rgb.at<cv::Vec3b>(v, u)[0]);
          po->g = (rgb.at<cv::Vec3b>(v, u)[1]);
          po->r = (rgb.at<cv::Vec3b>(v, u)[2]);
      }

  }
}


void generateColorMapNoEkf(sensor_msgs::ImagePtr msg_rgb, 
                    pcl::PointCloud<PointType>::Ptr& pc,
                    Eigen::Matrix3d& extrinsic_r, 
                    Eigen::Vector3d& extrinsic_t,
                    pcl::PointCloud<pcl::PointXYZRGB>::Ptr& pc_color)
{

    cv::Mat rgb = cv_bridge::toCvCopy(*msg_rgb, "bgr8")->image;
    cv::Scalar point_color(0, 0, 0); 
    vector<vector<int>> vu;
    
    
    // // print K_camera
    // ROS_WARN("K_camera: %lf %lf %lf %lf %lf %lf %lf %lf %lf", K_camera[0], K_camera[1], K_camera[2], K_camera[3], K_camera[4], K_camera[5], K_camera[6], K_camera[7], K_camera[8]);
    // // print extrinsics
    // ROS_WARN("extrinsic_r: %lf %lf %lf %lf %lf %lf %lf %lf %lf", extrinsic_r(0,0), extrinsic_r(0,1), extrinsic_r(0,2), extrinsic_r(1,0), extrinsic_r(1,1), extrinsic_r(1,2), extrinsic_r(2,0), extrinsic_r(2,1), extrinsic_r(2,2));
    // ROS_WARN("extrinsic_t: %lf %lf %lf", extrinsic_t.x(), extrinsic_t.y(), extrinsic_t.z());
    // //image size
    // ROS_WARN("rgb size: %d %d", rgb.rows, rgb.cols);

    for (int i = 0; i < pc->points.size(); i++)
    {

        // ROS_WARN("point: %lf %lf %lf", pc->points[i].x, pc->points[i].y, pc->points[i].z);
        Eigen::Vector3d point_pc = {pc->points[i].x, pc->points[i].y, pc->points[i].z};
        Eigen::Vector3d point_camera = extrinsic_r * point_pc + extrinsic_t;
        
        
        if (point_camera.z() > 0)
        {
            Eigen::Vector2d point_2d = (point_camera.head<2>() / point_camera.z()).eval();
            Eigen::Vector2d point_2d_dis = distort(point_2d);
            int u = static_cast<int>(K_camera[0] * point_2d_dis.x() + K_camera[2]);
            int v = static_cast<int>(K_camera[4] * point_2d_dis.y() + K_camera[5]);

            // vu.push_back({v,u});
            // ROS_WARN("point_2d: %lf %lf", point_2d.x(), point_2d.y());
            // ROS_WARN("point_2d_dis: %lf %lf", point_2d_dis.x(), point_2d_dis.y());
            // ROS_WARN("u: %d, v: %d", u, v);
            if (u >= 0 && u < rgb.cols && v >= 0 && v < rgb.rows)
            {

                pcl::PointXYZRGB point_rgb;
                point_rgb.x = point_pc.x();
                point_rgb.y = point_pc.y();
                point_rgb.z = point_pc.z();
                point_rgb.b = (rgb.at<cv::Vec3b>(v, u)[0]);
                point_rgb.g = (rgb.at<cv::Vec3b>(v, u)[1]);
                point_rgb.r = (rgb.at<cv::Vec3b>(v, u)[2]);
                pc_color->push_back(point_rgb);

                    
            }
    
        }


    }
    


    ROS_WARN("pc_color size: %d", pc_color->size());
}




static void dll_error(laszip_POINTER laszip) {
    if (laszip) {
        laszip_CHAR* error;
        if (laszip_get_error(laszip, &error)) {
            fprintf(stderr, "DLL ERROR: getting error messages\n");
        }
        if (error) {
            fprintf(stderr, "DLL ERROR MESSAGE: %s\n", error);
        } else {
            fprintf(stderr, "DLL ERROR MESSAGE: unknown error\n");
        }
    }
}
// Function to print header information
void printHeaderInfo(const laszip_header* header) {
    std::cout << "Header Info:" << std::endl;
    // std::cout << "File Signature: " << header->file_signature << std::endl;
    std::cout << "File Source ID: " << header->file_source_ID << std::endl;
    std::cout << "Global Encoding: " << header->global_encoding << std::endl;
    std::cout << "Project ID GUID data: " << header->project_ID_GUID_data_1 << "-"
              << header->project_ID_GUID_data_2 << "-"
              << header->project_ID_GUID_data_3 << "-"
              << header->project_ID_GUID_data_4 << std::endl;
    std::cout << "Version Major.Minor: " << static_cast<int>(header->version_major) << "."
              << static_cast<int>(header->version_minor) << std::endl;
    std::cout << "System Identifier: " << header->system_identifier << std::endl;
    std::cout << "Generating Software: " << header->generating_software << std::endl;
    std::cout << "Number of Point Records: " << header->number_of_point_records << std::endl;
    std::cout << "Number of Points by Return: " << header->number_of_points_by_return[0] << " "
              << header->number_of_points_by_return[1] << " "
              << header->number_of_points_by_return[2] << std::endl;
    std::cout << "X Scale Factor: " << header->x_scale_factor << std::endl;
    std::cout << "Y Scale Factor: " << header->y_scale_factor << std::endl;
    std::cout << "Z Scale Factor: " << header->z_scale_factor << std::endl;
    std::cout << "X Offset: " << header->x_offset << std::endl;
    std::cout << "Y Offset: " << header->y_offset << std::endl;
    std::cout << "Z Offset: " << header->z_offset << std::endl;
    std::cout << "file_creation_day: " << header->file_creation_day << std::endl;
    std::cout << "file_creation_year: " << header->file_creation_year << std::endl;
    std::cout << "header_size: " << header->header_size << std::endl;
    std::cout << "max x: " << header->max_x << std::endl;
    std::cout << "min x: " << header->min_x << std::endl;
    std::cout << "max y: " << header->max_y << std::endl;
    std::cout << "min y: " << header->min_y << std::endl;
    std::cout << "max z: " << header->max_z << std::endl;
    std::cout << "min z: " << header->min_z << std::endl;

}

static void byebye(bool error=false, bool wait=false, laszip_POINTER laszip=0)
{
  if (error)
  {
    dll_error(laszip);
  }
  if (wait)
  {
    fprintf(stderr,"<press ENTER>\n");
    getc(stdin);
  }
  exit(error);
}
// Function to read LAS/LAZ file and print header
void readLASHeader(const char* filename) {
    laszip_POINTER laszip_reader;
    if (laszip_create(&laszip_reader)) {
        throw std::runtime_error("DLL ERROR: creating laszip reader");
    }

    laszip_BOOL is_compressed = false;
    if (laszip_open_reader(laszip_reader, filename, &is_compressed)) {
        char* err_msg = nullptr;
        laszip_get_error(laszip_reader, &err_msg);
        std::string error = "DLL ERROR: opening laszip reader for '";
        error += filename;
        error += "' - ";
        error += err_msg;
        laszip_destroy(laszip_reader);
        throw std::runtime_error(error);
    }

    laszip_header* header;
    if (laszip_get_header_pointer(laszip_reader, &header)) {
        laszip_destroy(laszip_reader);
        throw std::runtime_error("DLL ERROR: getting header pointer from laszip reader");
    }

    // Print the header information
    printHeaderInfo(header);

    // Clean up
    if (laszip_close_reader(laszip_reader)) {
        laszip_destroy(laszip_reader);
        throw std::runtime_error("DLL ERROR: closing laszip reader");
    }

    laszip_destroy(laszip_reader);
}
void pclRGBToLaszip(const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& cloud, const std::string& filename) {

    ROS_INFO("Trying to save: %s", filename.c_str());
    int num_points = cloud->size();
    // Variables to hold the min and max 3D coordinates
    pcl::PointXYZRGB minPt, maxPt;

    // Get the minimum and maximum points
    pcl::getMinMax3D(*cloud, minPt, maxPt);
    
    ROS_INFO("Max x: %f", maxPt.x);
    ROS_INFO("Max y: %f", maxPt.y);
    ROS_INFO("Max z: %f", maxPt.z);
    ROS_INFO("Min x: %f", minPt.x);
    ROS_INFO("Min y: %f", minPt.y);
    ROS_INFO("Min z: %f", minPt.z);
    ROS_INFO("Num pts: %d", num_points);

    laszip_POINTER laszip_writer;

    if (laszip_create(&laszip_writer))
    {
      fprintf(stderr,"DLL ERROR: creating laszip writer\n");
      byebye(true, 1);
    }

    // get a pointer to the header of the writer so we can populate it

    laszip_header* header;

    if (laszip_get_header_pointer(laszip_writer, &header))
    {
      fprintf(stderr,"DLL ERROR: getting header pointer from laszip writer\n");
      byebye(true, 1, laszip_writer);
    }

    // populate the header

    header->file_source_ID = 4711;
    header->global_encoding = (1<<0);             // see LAS specification for details
    header->version_major = 1;
    header->version_minor = 2;
    strncpy(header->system_identifier, "LASzip DLL example 3", 32);
    header->file_creation_day = 1;
    header->file_creation_year = 2024;
    header->point_data_format = 2;
    header->point_data_record_length = 28;
    header->number_of_point_records = num_points;
    header->number_of_points_by_return[0] = 0;
    header->number_of_points_by_return[1] = 0;
    header->max_x = maxPt.x;
    header->max_y = maxPt.y;
    header->max_z = maxPt.z;
    header->min_x = minPt.x;
    header->min_y = minPt.y;
    header->min_z = minPt.z;
    char* file_name_out = 0;
    file_name_out = strdup(filename.c_str());
    laszip_BOOL compress = (strstr(file_name_out, ".laz") != 0);

    if (laszip_open_writer(laszip_writer, file_name_out, compress))
    {
      fprintf(stderr,"DLL ERROR: opening laszip writer for '%s'\n", filename);
      byebye(true, 1, laszip_writer);
    }

    laszip_I64 p_count = 0;

    for(int i = 0; i < num_points; i++){
      pcl::PointXYZRGB pt = cloud->points[i];
      laszip_point* point;
      if (laszip_get_point_pointer(laszip_writer, &point))
      {
        fprintf(stderr,"DLL ERROR: getting point pointer from laszip writer\n");
        byebye(true, 1, laszip_writer);
      }
      
      laszip_F64 coordinates[3];
      coordinates[0] = pt.x;
      coordinates[1] = pt.y;
      coordinates[2] = pt.z;

      if (laszip_set_coordinates(laszip_writer, coordinates))
      {
        fprintf(stderr,"DLL ERROR: setting coordinates for point %I64d\n", p_count);
        byebye(true, 1, laszip_writer);
      }

      point->rgb[0] = pt.r;
      point->rgb[1] = pt.g;
      point->rgb[2] = pt.b;
      
      // point->intensity = pt.intensity;
      // point->red = pt->r;
      // point->green = pt->g;
      // point->blue = pt->b; 


      if (laszip_write_point(laszip_writer))
      {
        fprintf(stderr,"DLL ERROR: writing point %I64d\n", p_count);
        byebye(true, 1, laszip_writer);
      }
      p_count++;



    }

    if (laszip_get_point_count(laszip_writer, &p_count))
    {
      fprintf(stderr,"DLL ERROR: getting point count\n");
      byebye(true, 1, laszip_writer);
    }

    fprintf(stderr,"successfully written %I64d points\n", p_count);

    if (laszip_close_writer(laszip_writer))
    {
      fprintf(stderr,"DLL ERROR: closing laszip writer\n");
      byebye(true, 1, laszip_writer);
    }

    if (laszip_destroy(laszip_writer))
    {
      fprintf(stderr,"DLL ERROR: destroying laszip writer\n");
      byebye(true, 1);
    }
}

void pclToLaszip(const pcl::PointCloud<pcl::PointXYZINormal>::Ptr& cloud, const std::string& filename) {

    ROS_INFO("Trying to save: %s", filename.c_str());
    int num_points = cloud->size();
    // Variables to hold the min and max 3D coordinates
    pcl::PointXYZINormal minPt, maxPt;

    // Get the minimum and maximum points
    pcl::getMinMax3D(*cloud, minPt, maxPt);
    
    ROS_INFO("Max x: %f", maxPt.x);
    ROS_INFO("Max y: %f", maxPt.y);
    ROS_INFO("Max z: %f", maxPt.z);
    ROS_INFO("Min x: %f", minPt.x);
    ROS_INFO("Min y: %f", minPt.y);
    ROS_INFO("Min z: %f", minPt.z);
    ROS_INFO("Num pts: %d", num_points);

    laszip_POINTER laszip_writer;

    if (laszip_create(&laszip_writer))
    {
      fprintf(stderr,"DLL ERROR: creating laszip writer\n");
      byebye(true, 1);
    }

    // get a pointer to the header of the writer so we can populate it

    laszip_header* header;

    if (laszip_get_header_pointer(laszip_writer, &header))
    {
      fprintf(stderr,"DLL ERROR: getting header pointer from laszip writer\n");
      byebye(true, 1, laszip_writer);
    }

    // populate the header

    header->file_source_ID = 4711;
    header->global_encoding = (1<<0);             // see LAS specification for details
    header->version_major = 1;
    header->version_minor = 2;
    strncpy(header->system_identifier, "LASzip DLL example 3", 32);
    header->file_creation_day = 1;
    header->file_creation_year = 2024;
    header->point_data_format = 1;
    header->point_data_record_length = 28;
    header->number_of_point_records = num_points;
    header->number_of_points_by_return[0] = 0;
    header->number_of_points_by_return[1] = 0;
    header->max_x = maxPt.x;
    header->max_y = maxPt.y;
    header->max_z = maxPt.z;
    header->min_x = minPt.x;
    header->min_y = minPt.y;
    header->min_z = minPt.z;
    char* file_name_out = 0;
    file_name_out = strdup(filename.c_str());
    laszip_BOOL compress = (strstr(file_name_out, ".laz") != 0);

    if (laszip_open_writer(laszip_writer, file_name_out, compress))
    {
      fprintf(stderr,"DLL ERROR: opening laszip writer for '%s'\n", filename);
      byebye(true, 1, laszip_writer);
    }

    laszip_I64 p_count = 0;

    for(int i = 0; i < num_points; i++){
      pcl::PointXYZINormal pt = cloud->points[i];
      laszip_point* point;
      if (laszip_get_point_pointer(laszip_writer, &point))
      {
        fprintf(stderr,"DLL ERROR: getting point pointer from laszip writer\n");
        byebye(true, 1, laszip_writer);
      }
      
      laszip_F64 coordinates[3];
      coordinates[0] = pt.x;
      coordinates[1] = pt.y;
      coordinates[2] = pt.z;

      if (laszip_set_coordinates(laszip_writer, coordinates))
      {
        fprintf(stderr,"DLL ERROR: setting coordinates for point %I64d\n", p_count);
        byebye(true, 1, laszip_writer);
      }

      point->intensity = pt.intensity;
      // point->red = pt->r;
      // point->green = pt->g;
      // point->blue = pt->b; 


      if (laszip_write_point(laszip_writer))
      {
        fprintf(stderr,"DLL ERROR: writing point %I64d\n", p_count);
        byebye(true, 1, laszip_writer);
      }
      p_count++;



    }

    if (laszip_get_point_count(laszip_writer, &p_count))
    {
      fprintf(stderr,"DLL ERROR: getting point count\n");
      byebye(true, 1, laszip_writer);
    }

    fprintf(stderr,"successfully written %I64d points\n", p_count);

    if (laszip_close_writer(laszip_writer))
    {
      fprintf(stderr,"DLL ERROR: closing laszip writer\n");
      byebye(true, 1, laszip_writer);
    }

    if (laszip_destroy(laszip_writer))
    {
      fprintf(stderr,"DLL ERROR: destroying laszip writer\n");
      byebye(true, 1);
    }
}




// set initial guess
void transformAssociateToMap() {
  q_w_curr = q_wmap_wodom * q_wodom_curr;
  t_w_curr = q_wmap_wodom * t_wodom_curr + t_wmap_wodom;
}

void transformUpdate() {
  q_wmap_wodom = q_w_curr * q_wodom_curr.inverse();
  t_wmap_wodom = t_w_curr - q_wmap_wodom * t_wodom_curr;
}

void pointAssociateToMap(PointType const *const pi, PointType *const po) {
  Eigen::Vector3d point_curr(pi->x, pi->y, pi->z);
  Eigen::Vector3d point_w = q_w_curr * point_curr + t_w_curr;
  po->x = point_w.x();
  po->y = point_w.y();
  po->z = point_w.z();
  po->intensity = pi->intensity;
  // po->intensity = 1.0;
}

void IntensityAssociateToMap(PointType const *const pi, pcl::PointXYZINormal *const po) {
  // printf("pi: %f %f %f %f\n", pi->x, pi->y, pi->z, pi->intensity);

  Eigen::Vector3d point_curr(pi->x, pi->y, pi->z);
  Eigen::Vector3d point_w = q_w_curr * point_curr + t_w_curr;
  po->x = point_w.x();
  po->y = point_w.y();
  po->z = point_w.z();
  po->intensity = pi->curvature*10;
  // po->intensity = 1.0;
}







void RGBpointAssociateToMap(PointType const *const pi,
                            pcl::PointXYZRGB *const po) {
  Eigen::Vector3d point_curr(pi->x, pi->y, pi->z);
  Eigen::Vector3d point_w = q_w_curr * point_curr + t_w_curr;
  po->x = point_w.x();
  po->y = point_w.y();
  po->z = point_w.z();
  int reflection_map = pi->curvature * 10;
  if (reflection_map < 30) {
    int green = (reflection_map * 255 / 30);
    po->r = 0;
    po->g = green & 0xff;
    po->b = 0xff;
  } else if (reflection_map < 90) {
    int blue = (((90 - reflection_map) * 255) / 60);
    po->r = 0x0;
    po->g = 0xff;
    po->b = blue & 0xff;
  } else if (reflection_map < 150) {
    int red = ((reflection_map - 90) * 255 / 60);
    po->r = red & 0xff;
    po->g = 0xff;
    po->b = 0x0;
  } else {
    int green = (((255 - reflection_map) * 255) / (255 - 150));
    po->r = 0xff;
    po->g = green & 0xff;
    po->b = 0;
  }
}

void pointAssociateTobeMapped(PointType const *const pi, PointType *const po) {
  Eigen::Vector3d point_w(pi->x, pi->y, pi->z);
  Eigen::Vector3d point_curr = q_w_curr.inverse() * (point_w - t_w_curr);
  po->x = point_curr.x();
  po->y = point_curr.y();
  po->z = point_curr.z();
  po->intensity = pi->intensity;
}

void laserCloudCornerLastHandler(
    const sensor_msgs::PointCloud2ConstPtr &laserCloudCornerLast2) {
  mBuf.lock();
  cornerLastBuf.push(laserCloudCornerLast2);
  mBuf.unlock();
}

void laserCloudSurfLastHandler(
    const sensor_msgs::PointCloud2ConstPtr &laserCloudSurfLast2) {
  mBuf.lock();
  surfLastBuf.push(laserCloudSurfLast2);
  mBuf.unlock();
}

void laserCloudFullResHandler(
    const sensor_msgs::PointCloud2ConstPtr &laserCloudFullRes2) {
  mBuf.lock();

  // print msg header info:
  std::cout << "header time: " << laserCloudFullRes2->header.stamp << std::endl;

  fullResBuf.push(laserCloudFullRes2);
  mBuf.unlock();
}

void cameraHandler(
    const sensor_msgs::ImageConstPtr &msg) {
  
  // mCam.lock();
  ros::Time msg_time = msg->header.stamp;
  sensor_msgs::ImagePtr image_msg(new sensor_msgs::Image);
  *image_msg = *msg;
  image_msg->header.stamp = ros::Time().fromSec(msg_time.toSec());
  camera_buffer.push_back(image_msg);
  camera_time_buffer.push_back(msg_time.toSec());
  // mCam.unlock();
  // sig_cam_buffer.notify_all();
  ROS_WARN("IMAGE PUBLISHED");
}





// receive odomtry
void laserOdometryHandler(const nav_msgs::Odometry::ConstPtr &laserOdometry) {
  mBuf.lock();
  odometryBuf.push(laserOdometry);
  mBuf.unlock();

  // high frequence publish
  Eigen::Quaterniond q_wodom_curr;
  Eigen::Vector3d t_wodom_curr;
  q_wodom_curr.x() = laserOdometry->pose.pose.orientation.x;
  q_wodom_curr.y() = laserOdometry->pose.pose.orientation.y;
  q_wodom_curr.z() = laserOdometry->pose.pose.orientation.z;
  q_wodom_curr.w() = laserOdometry->pose.pose.orientation.w;
  t_wodom_curr.x() = laserOdometry->pose.pose.position.x;
  t_wodom_curr.y() = laserOdometry->pose.pose.position.y;
  t_wodom_curr.z() = laserOdometry->pose.pose.position.z;

  Eigen::Quaterniond q_w_curr = q_wmap_wodom * q_wodom_curr;
  Eigen::Vector3d t_w_curr = q_wmap_wodom * t_wodom_curr + t_wmap_wodom;

  nav_msgs::Odometry odomAftMapped;
  odomAftMapped.header.frame_id = "/camera_init";
  odomAftMapped.child_frame_id = "/aft_mapped";
  odomAftMapped.header.stamp = laserOdometry->header.stamp;
  odomAftMapped.pose.pose.orientation.x = q_w_curr.x();
  odomAftMapped.pose.pose.orientation.y = q_w_curr.y();
  odomAftMapped.pose.pose.orientation.z = q_w_curr.z();
  odomAftMapped.pose.pose.orientation.w = q_w_curr.w();
  odomAftMapped.pose.pose.position.x = t_w_curr.x();
  odomAftMapped.pose.pose.position.y = t_w_curr.y();
  odomAftMapped.pose.pose.position.z = t_w_curr.z();
  pubOdomAftMappedHighFrec.publish(odomAftMapped);
}

void process() {
  while (1) {
    while (!cornerLastBuf.empty() && !surfLastBuf.empty() &&
           !fullResBuf.empty() && !odometryBuf.empty()) {
      mBuf.lock();
      while (!odometryBuf.empty() &&
             odometryBuf.front()->header.stamp.toSec() <
                 cornerLastBuf.front()->header.stamp.toSec())
        odometryBuf.pop();
      if (odometryBuf.empty()) {
        mBuf.unlock();
        break;
      }

      while (!surfLastBuf.empty() &&
             surfLastBuf.front()->header.stamp.toSec() <
                 cornerLastBuf.front()->header.stamp.toSec())
        surfLastBuf.pop();
      if (surfLastBuf.empty()) {
        mBuf.unlock();
        break;
      }

      while (!fullResBuf.empty() &&
             fullResBuf.front()->header.stamp.toSec() <
                 cornerLastBuf.front()->header.stamp.toSec())
        fullResBuf.pop();
      if (fullResBuf.empty()) {
        mBuf.unlock();
        break;
      }

      timeLaserCloudCornerLast = cornerLastBuf.front()->header.stamp.toSec();
      timeLaserCloudSurfLast = surfLastBuf.front()->header.stamp.toSec();
      timeLaserCloudFullRes = fullResBuf.front()->header.stamp.toSec();
      timeLaserOdometry = odometryBuf.front()->header.stamp.toSec();

      if (timeLaserCloudCornerLast != timeLaserOdometry ||
          timeLaserCloudSurfLast != timeLaserOdometry ||
          timeLaserCloudFullRes != timeLaserOdometry) {
        ROS_INFO("time corner %f surf %f full %f odom %f \n",
               timeLaserCloudCornerLast, timeLaserCloudSurfLast,
               timeLaserCloudFullRes, timeLaserOdometry);
        ROS_INFO("unsync messeage!");
        mBuf.unlock();
        break;
      }

      laserCloudCornerLast->clear();
      pcl::fromROSMsg(*cornerLastBuf.front(), *laserCloudCornerLast);
      cornerLastBuf.pop();

      laserCloudSurfLast->clear();
      pcl::fromROSMsg(*surfLastBuf.front(), *laserCloudSurfLast);
      surfLastBuf.pop();

      laserCloudFullRes->clear();
      pcl::fromROSMsg(*fullResBuf.front(), *laserCloudFullRes);
      fullResBuf.pop();

      q_wodom_curr.x() = odometryBuf.front()->pose.pose.orientation.x;
      q_wodom_curr.y() = odometryBuf.front()->pose.pose.orientation.y;
      q_wodom_curr.z() = odometryBuf.front()->pose.pose.orientation.z;
      q_wodom_curr.w() = odometryBuf.front()->pose.pose.orientation.w;
      t_wodom_curr.x() = odometryBuf.front()->pose.pose.position.x;
      t_wodom_curr.y() = odometryBuf.front()->pose.pose.position.y;
      t_wodom_curr.z() = odometryBuf.front()->pose.pose.position.z;
      odometryBuf.pop();

      //      while (!cornerLastBuf.empty()) {
      //        //cornerLastBuf.pop();
      //        printf("drop lidar frame in mapping for real time performance
      //        \n");
      //      }


      mBuf.unlock();

      TicToc t_whole;

      transformAssociateToMap();

      TicToc t_shift;
      int centerCubeI = int((t_w_curr.x() + 25.0) / 50.0) + laserCloudCenWidth;
      int centerCubeJ = int((t_w_curr.y() + 25.0) / 50.0) + laserCloudCenHeight;
      int centerCubeK = int((t_w_curr.z() + 25.0) / 50.0) + laserCloudCenDepth;

      if (t_w_curr.x() + 25.0 < 0) centerCubeI--;
      if (t_w_curr.y() + 25.0 < 0) centerCubeJ--;
      if (t_w_curr.z() + 25.0 < 0) centerCubeK--;

      while (centerCubeI < 3) {
        for (int j = 0; j < laserCloudHeight; j++) {
          for (int k = 0; k < laserCloudDepth; k++) {
            int i = laserCloudWidth - 1;
            pcl::PointCloud<PointType>::Ptr laserCloudCubeCornerPointer =
                laserCloudCornerArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                laserCloudSurfArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k];
            for (; i >= 1; i--) {
              laserCloudCornerArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k] =
                  laserCloudCornerArray[i - 1 + laserCloudWidth * j +
                                        laserCloudWidth * laserCloudHeight * k];
              laserCloudSurfArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                  laserCloudSurfArray[i - 1 + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            }
            laserCloudCornerArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeCornerPointer;
            laserCloudSurfArray[i + laserCloudWidth * j +
                                laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeSurfPointer;
            laserCloudCubeCornerPointer->clear();
            laserCloudCubeSurfPointer->clear();
          }
        }

        centerCubeI++;
        laserCloudCenWidth++;
      }

      while (centerCubeI >= laserCloudWidth - 3) {
        for (int j = 0; j < laserCloudHeight; j++) {
          for (int k = 0; k < laserCloudDepth; k++) {
            int i = 0;
            pcl::PointCloud<PointType>::Ptr laserCloudCubeCornerPointer =
                laserCloudCornerArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                laserCloudSurfArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k];
            for (; i < laserCloudWidth - 1; i++) {
              laserCloudCornerArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k] =
                  laserCloudCornerArray[i + 1 + laserCloudWidth * j +
                                        laserCloudWidth * laserCloudHeight * k];
              laserCloudSurfArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                  laserCloudSurfArray[i + 1 + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            }
            laserCloudCornerArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeCornerPointer;
            laserCloudSurfArray[i + laserCloudWidth * j +
                                laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeSurfPointer;
            laserCloudCubeCornerPointer->clear();
            laserCloudCubeSurfPointer->clear();
          }
        }

        centerCubeI--;
        laserCloudCenWidth--;
      }

      while (centerCubeJ < 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
          for (int k = 0; k < laserCloudDepth; k++) {
            int j = laserCloudHeight - 1;
            pcl::PointCloud<PointType>::Ptr laserCloudCubeCornerPointer =
                laserCloudCornerArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                laserCloudSurfArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k];
            for (; j >= 1; j--) {
              laserCloudCornerArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k] =
                  laserCloudCornerArray[i + laserCloudWidth * (j - 1) +
                                        laserCloudWidth * laserCloudHeight * k];
              laserCloudSurfArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                  laserCloudSurfArray[i + laserCloudWidth * (j - 1) +
                                      laserCloudWidth * laserCloudHeight * k];
            }
            laserCloudCornerArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeCornerPointer;
            laserCloudSurfArray[i + laserCloudWidth * j +
                                laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeSurfPointer;
            laserCloudCubeCornerPointer->clear();
            laserCloudCubeSurfPointer->clear();
          }
        }

        centerCubeJ++;
        laserCloudCenHeight++;
      }

      while (centerCubeJ >= laserCloudHeight - 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
          for (int k = 0; k < laserCloudDepth; k++) {
            int j = 0;
            pcl::PointCloud<PointType>::Ptr laserCloudCubeCornerPointer =
                laserCloudCornerArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                laserCloudSurfArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k];
            for (; j < laserCloudHeight - 1; j++) {
              laserCloudCornerArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k] =
                  laserCloudCornerArray[i + laserCloudWidth * (j + 1) +
                                        laserCloudWidth * laserCloudHeight * k];
              laserCloudSurfArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                  laserCloudSurfArray[i + laserCloudWidth * (j + 1) +
                                      laserCloudWidth * laserCloudHeight * k];
            }
            laserCloudCornerArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeCornerPointer;
            laserCloudSurfArray[i + laserCloudWidth * j +
                                laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeSurfPointer;
            laserCloudCubeCornerPointer->clear();
            laserCloudCubeSurfPointer->clear();
          }
        }

        centerCubeJ--;
        laserCloudCenHeight--;
      }

      while (centerCubeK < 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
          for (int j = 0; j < laserCloudHeight; j++) {
            int k = laserCloudDepth - 1;
            pcl::PointCloud<PointType>::Ptr laserCloudCubeCornerPointer =
                laserCloudCornerArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                laserCloudSurfArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k];
            for (; k >= 1; k--) {
              laserCloudCornerArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k] =
                  laserCloudCornerArray[i + laserCloudWidth * j +
                                        laserCloudWidth * laserCloudHeight *
                                            (k - 1)];
              laserCloudSurfArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                  laserCloudSurfArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight *
                                          (k - 1)];
            }
            laserCloudCornerArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeCornerPointer;
            laserCloudSurfArray[i + laserCloudWidth * j +
                                laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeSurfPointer;
            laserCloudCubeCornerPointer->clear();
            laserCloudCubeSurfPointer->clear();
          }
        }

        centerCubeK++;
        laserCloudCenDepth++;
      }

      while (centerCubeK >= laserCloudDepth - 3) {
        for (int i = 0; i < laserCloudWidth; i++) {
          for (int j = 0; j < laserCloudHeight; j++) {
            int k = 0;
            pcl::PointCloud<PointType>::Ptr laserCloudCubeCornerPointer =
                laserCloudCornerArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight * k];
            pcl::PointCloud<PointType>::Ptr laserCloudCubeSurfPointer =
                laserCloudSurfArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k];
            for (; k < laserCloudDepth - 1; k++) {
              laserCloudCornerArray[i + laserCloudWidth * j +
                                    laserCloudWidth * laserCloudHeight * k] =
                  laserCloudCornerArray[i + laserCloudWidth * j +
                                        laserCloudWidth * laserCloudHeight *
                                            (k + 1)];
              laserCloudSurfArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                  laserCloudSurfArray[i + laserCloudWidth * j +
                                      laserCloudWidth * laserCloudHeight *
                                          (k + 1)];
            }
            laserCloudCornerArray[i + laserCloudWidth * j +
                                  laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeCornerPointer;
            laserCloudSurfArray[i + laserCloudWidth * j +
                                laserCloudWidth * laserCloudHeight * k] =
                laserCloudCubeSurfPointer;
            laserCloudCubeCornerPointer->clear();
            laserCloudCubeSurfPointer->clear();
          }
        }

        centerCubeK--;
        laserCloudCenDepth--;
      }

      int laserCloudValidNum = 0;
      int laserCloudSurroundNum = 0;

      for (int i = centerCubeI - 2; i <= centerCubeI + 2; i++) {
        for (int j = centerCubeJ - 2; j <= centerCubeJ + 2; j++) {
          for (int k = centerCubeK - 1; k <= centerCubeK + 1; k++) {
            if (i >= 0 && i < laserCloudWidth && j >= 0 &&
                j < laserCloudHeight && k >= 0 && k < laserCloudDepth) {
              laserCloudValidInd[laserCloudValidNum] =
                  i + laserCloudWidth * j +
                  laserCloudWidth * laserCloudHeight * k;
              laserCloudValidNum++;
              laserCloudSurroundInd[laserCloudSurroundNum] =
                  i + laserCloudWidth * j +
                  laserCloudWidth * laserCloudHeight * k;
              laserCloudSurroundNum++;
            }
          }
        }
      }

      laserCloudCornerFromMap->clear();
      laserCloudSurfFromMap->clear();
      for (int i = 0; i < laserCloudValidNum; i++) {
        *laserCloudCornerFromMap +=
            *laserCloudCornerArray[laserCloudValidInd[i]];
        *laserCloudSurfFromMap += *laserCloudSurfArray[laserCloudValidInd[i]];
      }
      int laserCloudCornerFromMapNum = laserCloudCornerFromMap->points.size();
      int laserCloudSurfFromMapNum = laserCloudSurfFromMap->points.size();

      pcl::PointCloud<PointType>::Ptr laserCloudCornerStack(
          new pcl::PointCloud<PointType>());
      downSizeFilterCorner.setInputCloud(laserCloudCornerLast);
      downSizeFilterCorner.filter(*laserCloudCornerStack);
      int laserCloudCornerStackNum = laserCloudCornerStack->points.size();

      pcl::PointCloud<PointType>::Ptr laserCloudSurfStack(
          new pcl::PointCloud<PointType>());
      downSizeFilterSurf.setInputCloud(laserCloudSurfLast);
      downSizeFilterSurf.filter(*laserCloudSurfStack);
      int laserCloudSurfStackNum = laserCloudSurfStack->points.size();

      ROS_INFO("map prepare time %f ms\n", t_shift.toc());
      ROS_INFO("map corner num %d  surf num %d \n", laserCloudCornerFromMapNum,
             laserCloudSurfFromMapNum);
      if (laserCloudCornerFromMapNum > 10 && laserCloudSurfFromMapNum > 50) {
        TicToc t_opt;
        TicToc t_tree;
        kdtreeCornerFromMap->setInputCloud(laserCloudCornerFromMap);
        kdtreeSurfFromMap->setInputCloud(laserCloudSurfFromMap);
        ROS_INFO("build tree time %f ms \n", t_tree.toc());

        for (int iterCount = 0; iterCount < 2; iterCount++) {
          // ceres::LossFunction *loss_function = NULL;
          ceres::LossFunction *loss_function = new ceres::HuberLoss(0.1);
          ceres::LocalParameterization *q_parameterization =
              new ceres::EigenQuaternionParameterization();
          ceres::Problem::Options problem_options;

          ceres::Problem problem(problem_options);
          problem.AddParameterBlock(parameters, 4, q_parameterization);
          problem.AddParameterBlock(parameters + 4, 3);

          TicToc t_data;
          int corner_num = 0;

          for (int i = 0; i < laserCloudCornerStackNum; i++) {
            pointOri = laserCloudCornerStack->points[i];
            // double sqrtDis = pointOri.x * pointOri.x + pointOri.y *
            // pointOri.y + pointOri.z * pointOri.z;
            pointAssociateToMap(&pointOri, &pointSel);
            kdtreeCornerFromMap->nearestKSearch(pointSel, 5, pointSearchInd,
                                                pointSearchSqDis);

            if (pointSearchSqDis[4] < 1.0) {
              std::vector<Eigen::Vector3d> nearCorners;
              Eigen::Vector3d center(0, 0, 0);
              for (int j = 0; j < 5; j++) {
                Eigen::Vector3d tmp(
                    laserCloudCornerFromMap->points[pointSearchInd[j]].x,
                    laserCloudCornerFromMap->points[pointSearchInd[j]].y,
                    laserCloudCornerFromMap->points[pointSearchInd[j]].z);
                center = center + tmp;
                nearCorners.push_back(tmp);
              }
              center = center / 5.0;

              Eigen::Matrix3d covMat = Eigen::Matrix3d::Zero();
              for (int j = 0; j < 5; j++) {
                Eigen::Matrix<double, 3, 1> tmpZeroMean =
                    nearCorners[j] - center;
                covMat = covMat + tmpZeroMean * tmpZeroMean.transpose();
              }

              Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> saes(covMat);

              // if is indeed line feature
              // note Eigen library sort eigenvalues in increasing order
              Eigen::Vector3d unit_direction = saes.eigenvectors().col(2);
              Eigen::Vector3d curr_point(pointOri.x, pointOri.y, pointOri.z);
              if (saes.eigenvalues()[2] > 3 * saes.eigenvalues()[1]) {
                Eigen::Vector3d point_on_line = center;
                Eigen::Vector3d point_a, point_b;
                point_a = 0.1 * unit_direction + point_on_line;
                point_b = -0.1 * unit_direction + point_on_line;

                ceres::CostFunction *cost_function =
                    LidarEdgeFactor::Create(curr_point, point_a, point_b, 1.0);
                problem.AddResidualBlock(cost_function, loss_function,
                                         parameters, parameters + 4);
                corner_num++;
              }
            }
            /*
            else if(pointSearchSqDis[4] < 0.01 * sqrtDis)
            {
                    Eigen::Vector3d center(0, 0, 0);
                    for (int j = 0; j < 5; j++)
                    {
                            Eigen::Vector3d
            tmp(laserCloudCornerFromMap->points[pointSearchInd[j]].x,
                                                                    laserCloudCornerFromMap->points[pointSearchInd[j]].y,
                                                                    laserCloudCornerFromMap->points[pointSearchInd[j]].z);
                            center = center + tmp;
                    }
                    center = center / 5.0;
                    Eigen::Vector3d curr_point(pointOri.x, pointOri.y,
            pointOri.z);
                    ceres::CostFunction *cost_function =
            LidarDistanceFactor::Create(curr_point, center);
                    problem.AddResidualBlock(cost_function, loss_function,
            parameters, parameters + 4);
            }
            */
          }

          int surf_num = 0;
          for (int i = 0; i < laserCloudSurfStackNum; i++) {
            pointOri = laserCloudSurfStack->points[i];
            // double sqrtDis = pointOri.x * pointOri.x + pointOri.y *
            // pointOri.y + pointOri.z * pointOri.z;
            pointAssociateToMap(&pointOri, &pointSel);
            kdtreeSurfFromMap->nearestKSearch(pointSel, 5, pointSearchInd,
                                              pointSearchSqDis);

            Eigen::Matrix<double, 5, 3> matA0;
            Eigen::Matrix<double, 5, 1> matB0 =
                -1 * Eigen::Matrix<double, 5, 1>::Ones();
            if (pointSearchSqDis[4] < 1.0) {
              for (int j = 0; j < 5; j++) {
                matA0(j, 0) =
                    laserCloudSurfFromMap->points[pointSearchInd[j]].x;
                matA0(j, 1) =
                    laserCloudSurfFromMap->points[pointSearchInd[j]].y;
                matA0(j, 2) =
                    laserCloudSurfFromMap->points[pointSearchInd[j]].z;
                // printf(" pts %f %f %f ", matA0(j, 0), matA0(j, 1), matA0(j,
                // 2));
              }
              // find the norm of plane
              Eigen::Vector3d norm = matA0.colPivHouseholderQr().solve(matB0);
              double negative_OA_dot_norm = 1 / norm.norm();
              norm.normalize();

              // Here n(pa, pb, pc) is unit norm of plane
              bool planeValid = true;
              for (int j = 0; j < 5; j++) {
                // if OX * n > 0.2, then plane is not fit well
                if (fabs(
                        norm(0) *
                            laserCloudSurfFromMap->points[pointSearchInd[j]].x +
                        norm(1) *
                            laserCloudSurfFromMap->points[pointSearchInd[j]].y +
                        norm(2) *
                            laserCloudSurfFromMap->points[pointSearchInd[j]].z +
                        negative_OA_dot_norm) > 0.2) {
                  planeValid = false;
                  break;
                }
              }
              Eigen::Vector3d curr_point(pointOri.x, pointOri.y, pointOri.z);
              if (planeValid) {
                ceres::CostFunction *cost_function =
                    LidarPlaneNormFactor::Create(curr_point, norm,
                                                 negative_OA_dot_norm);
                problem.AddResidualBlock(cost_function, loss_function,
                                         parameters, parameters + 4);
                surf_num++;
              }
            }
            /*
            else if(pointSearchSqDis[4] < 0.01 * sqrtDis)
            {
                    Eigen::Vector3d center(0, 0, 0);
                    for (int j = 0; j < 5; j++)
                    {
                            Eigen::Vector3d
            tmp(laserCloudSurfFromMap->points[pointSearchInd[j]].x,
                                                                    laserCloudSurfFromMap->points[pointSearchInd[j]].y,
                                                                    laserCloudSurfFromMap->points[pointSearchInd[j]].z);
                            center = center + tmp;
                    }
                    center = center / 5.0;
                    Eigen::Vector3d curr_point(pointOri.x, pointOri.y,
            pointOri.z);
                    ceres::CostFunction *cost_function =
            LidarDistanceFactor::Create(curr_point, center);
                    problem.AddResidualBlock(cost_function, loss_function,
            parameters, parameters + 4);
            }
            */
          }

          // printf("corner num %d used corner num %d \n",
          // laserCloudCornerStackNum, corner_num);
          // printf("surf num %d used surf num %d \n", laserCloudSurfStackNum,
          // surf_num);

          ROS_INFO("mapping data assosiation time %f ms \n", t_data.toc());

          TicToc t_solver;
          ceres::Solver::Options options;
          options.linear_solver_type = ceres::DENSE_QR;
          options.max_num_iterations = 10;
          options.minimizer_progress_to_stdout = false;
          options.check_gradients = false;
          options.gradient_check_relative_precision = 1e-4;
          ceres::Solver::Summary summary;
          ceres::Solve(options, &problem, &summary);
          ROS_INFO("mapping solver time %f ms \n", t_solver.toc());
          std::cout << summary.BriefReport() << std::endl;
          // printf("time %f \n", timeLaserOdometry);
          // printf("corner factor num %d surf factor num %d\n", corner_num,
          // surf_num);
          // printf("result q %f %f %f %f result t %f %f %f\n", parameters[3],
          // parameters[0], parameters[1], parameters[2],
          //	   parameters[4], parameters[5], parameters[6]);
        }
        ROS_INFO("mapping optimization time %f \n", t_opt.toc());
      } else {
        ROS_WARN("time Map corner and surf num are not enough");
      }
      transformUpdate();

      TicToc t_add;
      for (int i = 0; i < laserCloudCornerStackNum; i++) {
        pointAssociateToMap(&laserCloudCornerStack->points[i], &pointSel);

        int cubeI = int((pointSel.x + 25.0) / 50.0) + laserCloudCenWidth;
        int cubeJ = int((pointSel.y + 25.0) / 50.0) + laserCloudCenHeight;
        int cubeK = int((pointSel.z + 25.0) / 50.0) + laserCloudCenDepth;

        if (pointSel.x + 25.0 < 0) cubeI--;
        if (pointSel.y + 25.0 < 0) cubeJ--;
        if (pointSel.z + 25.0 < 0) cubeK--;

        if (cubeI >= 0 && cubeI < laserCloudWidth && cubeJ >= 0 &&
            cubeJ < laserCloudHeight && cubeK >= 0 && cubeK < laserCloudDepth) {
          int cubeInd = cubeI + laserCloudWidth * cubeJ +
                        laserCloudWidth * laserCloudHeight * cubeK;
          laserCloudCornerArray[cubeInd]->push_back(pointSel);
        }
      }

      for (int i = 0; i < laserCloudSurfStackNum; i++) {
        pointAssociateToMap(&laserCloudSurfStack->points[i], &pointSel);

        int cubeI = int((pointSel.x + 25.0) / 50.0) + laserCloudCenWidth;
        int cubeJ = int((pointSel.y + 25.0) / 50.0) + laserCloudCenHeight;
        int cubeK = int((pointSel.z + 25.0) / 50.0) + laserCloudCenDepth;

        if (pointSel.x + 25.0 < 0) cubeI--;
        if (pointSel.y + 25.0 < 0) cubeJ--;
        if (pointSel.z + 25.0 < 0) cubeK--;

        if (cubeI >= 0 && cubeI < laserCloudWidth && cubeJ >= 0 &&
            cubeJ < laserCloudHeight && cubeK >= 0 && cubeK < laserCloudDepth) {
          int cubeInd = cubeI + laserCloudWidth * cubeJ +
                        laserCloudWidth * laserCloudHeight * cubeK;
          laserCloudSurfArray[cubeInd]->push_back(pointSel);
        }
      }
      ROS_INFO("add points time %f ms\n", t_add.toc());

      TicToc t_filter;
      for (int i = 0; i < laserCloudValidNum; i++) {
        int ind = laserCloudValidInd[i];

        pcl::PointCloud<PointType>::Ptr tmpCorner(
            new pcl::PointCloud<PointType>());
        downSizeFilterCorner.setInputCloud(laserCloudCornerArray[ind]);
        downSizeFilterCorner.filter(*tmpCorner);
        laserCloudCornerArray[ind] = tmpCorner;

        pcl::PointCloud<PointType>::Ptr tmpSurf(
            new pcl::PointCloud<PointType>());
        downSizeFilterSurf.setInputCloud(laserCloudSurfArray[ind]);
        downSizeFilterSurf.filter(*tmpSurf);
        laserCloudSurfArray[ind] = tmpSurf;
      }
      ROS_INFO("filter time %f ms \n", t_filter.toc());

      TicToc t_pub;
      // publish surround map for every 5 frame
      if (frameCount % 5 == 0) {
        laserCloudSurround->clear();
        for (int i = 0; i < laserCloudSurroundNum; i++) {
          int ind = laserCloudSurroundInd[i];
          *laserCloudSurround += *laserCloudCornerArray[ind];
          *laserCloudSurround += *laserCloudSurfArray[ind];
        }

        sensor_msgs::PointCloud2 laserCloudSurround3;
        pcl::toROSMsg(*laserCloudSurround, laserCloudSurround3);
        laserCloudSurround3.header.stamp =
            ros::Time().fromSec(timeLaserOdometry);
        laserCloudSurround3.header.frame_id = "/camera_init";
        pubLaserCloudSurround.publish(laserCloudSurround3);
      }

      if (frameCount % 20 == 0) {
        pcl::PointCloud<PointType> laserCloudMap;
        for (int i = 0; i < 4851; i++) {
          laserCloudMap += *laserCloudCornerArray[i];
          laserCloudMap += *laserCloudSurfArray[i];
        }
        sensor_msgs::PointCloud2 laserCloudMsg;
        pcl::toROSMsg(laserCloudMap, laserCloudMsg);
        laserCloudMsg.header.stamp = ros::Time().fromSec(timeLaserOdometry);
        laserCloudMsg.header.frame_id = "/camera_init";
        pubLaserCloudMap.publish(laserCloudMsg);
      }

      laserCloudFullResColor->clear();
      int laserCloudFullResNum = laserCloudFullRes->points.size();
      for (int i = 0; i < laserCloudFullResNum; i++) {
        pcl::PointXYZRGB temp_point;
        RGBpointAssociateToMap(&laserCloudFullRes->points[i], &temp_point);
        laserCloudFullResColor->push_back(temp_point);
      }


      if (use_color){
        int camera_id = -1;
        laserColorFullRes->clear();
        TicToc t_coloring;
        
        if (find_best_camera_match(timeLaserCloudFullRes, camera_id)) {
          ROS_INFO("camera_id: %d \n", camera_id);
          ROS_INFO("lidar time: %f ms \n", timeLaserCloudFullRes);
          ROS_WARN("camera buffer size %d \n", camera_buffer.size());
          
          cv::Mat rgb = cv_bridge::toCvCopy(*camera_buffer[camera_id], "bgr8")->image;

          for (int i = 0; i < laserCloudFullResNum; i++) {
            pcl::PointXYZRGB temp_point;
            CameraRGBAssociateToMap(&laserCloudFullRes->points[i], &temp_point, rgb);
            laserColorFullRes->push_back(temp_point);
          }




          // generateColorMapNoEkf(camera_buffer[camera_id], laserCloudFullRes, Camera_R_wrt_Lidar, Camera_T_wrt_Lidar, pc_color);

          for (int i = 0; i <= camera_id; i++)
          {
              camera_time_buffer.pop_front();
              camera_buffer.pop_front();
          }

          // sensor_msgs::PointCloud2 laserRGBCloudMsg;
          // pcl::toROSMsg(*laserColorFullResIntensity, laserRGBCloudMsg);
          // laserRGBCloudMsg.header.stamp = ros::Time().fromSec(timeLaserCloudFullRes);
          // laserRGBCloudMsg.header.frame_id = "/camera_init";
          // pubLaserColor.publish(laserRGBCloudMsg);
          

          *laserColorCloudWaitSave += *laserColorFullRes;
          // save pc_color, filename is timestamp
          // pcl::PCDWriter pcd_writer;
          // pcd_writer.writeBinary("/home/gabriel/loam_horizon_ws/src/livox_horizon_loam/PCD/color/" 
          // + std::to_string(timeLaserCloudFullRes) + ".pcd", *pc_color);


        } else {
          ROS_INFO("no camera match \n");
        }

        ROS_WARN("coloring time %f ms \n", t_coloring.toc());    
      }

      else{
        laserCloudFullResIntensity->clear();
        
        for (int i = 0; i < laserCloudFullResNum; i++) {
          PointType temp_point;
          IntensityAssociateToMap(&laserCloudFullRes->points[i], &temp_point);
          laserCloudFullResIntensity->push_back(temp_point);
        }
        *laserCloudWaitSave += *laserCloudFullResIntensity;

      }



      // laserCloudFullResIntensity->clear();
      
      // for (int i = 0; i < laserCloudFullResNum; i++) {
      //   pcl::PointXYZINormal temp_point;
      //   IntensityAssociateToMap(&laserCloudFullRes->points[i], &temp_point);
      //   laserCloudFullResIntensity->push_back(temp_point);
      // }


      


      sensor_msgs::PointCloud2 laserCloudFullRes3;
      pcl::toROSMsg(*laserCloudFullResColor, laserCloudFullRes3);
      laserCloudFullRes3.header.stamp = ros::Time().fromSec(timeLaserOdometry);
      laserCloudFullRes3.header.frame_id = "/camera_init";
      pubLaserCloudFullRes.publish(laserCloudFullRes3);


      // pcl::PCDWriter pcd_writer;
      // pcd_writer.writeBinary("/home/gabriel/loam_horizon_ws/src/livox_horizon_loam/PCD/registered/laserCloudMap_" 
      // + std::to_string(laserCloudFullRes3.header.stamp.toSec()) + ".pcd", *laserCloudFullResIntensity);

      // pcd_writer.writeBinary(std::string(ROOT_DIR)+"/PCD/raw/laserCloudMap_" 
      // + std::to_string(laserCloudFullRes3.header.stamp.toSec()) + ".pcd", *laserCloudFullRes);
      
      



      ROS_INFO("mapping pub time %f ms \n", t_pub.toc());

      ROS_INFO("whole mapping time %f ms +++++\n", t_whole.toc());

      nav_msgs::Odometry odomAftMapped;
      odomAftMapped.header.frame_id = "/camera_init";
      odomAftMapped.child_frame_id = "/aft_mapped";
      odomAftMapped.header.stamp = ros::Time().fromSec(timeLaserOdometry);
      odomAftMapped.pose.pose.orientation.x = q_w_curr.x();
      odomAftMapped.pose.pose.orientation.y = q_w_curr.y();
      odomAftMapped.pose.pose.orientation.z = q_w_curr.z();
      odomAftMapped.pose.pose.orientation.w = q_w_curr.w();
      odomAftMapped.pose.pose.position.x = t_w_curr.x();
      odomAftMapped.pose.pose.position.y = t_w_curr.y();
      odomAftMapped.pose.pose.position.z = t_w_curr.z();
      pubOdomAftMapped.publish(odomAftMapped);

      geometry_msgs::PoseStamped laserAfterMappedPose;
      laserAfterMappedPose.header = odomAftMapped.header;
      laserAfterMappedPose.pose = odomAftMapped.pose.pose;
      laserAfterMappedPath.header.stamp = odomAftMapped.header.stamp;
      laserAfterMappedPath.header.frame_id = "/camera_init";
      laserAfterMappedPath.poses.push_back(laserAfterMappedPose);
      pubLaserAfterMappedPath.publish(laserAfterMappedPath);

      static tf::TransformBroadcaster br;
      tf::Transform transform;
      tf::Quaternion q;
      transform.setOrigin(tf::Vector3(t_w_curr(0), t_w_curr(1), t_w_curr(2)));
      q.setW(q_w_curr.w());
      q.setX(q_w_curr.x());
      q.setY(q_w_curr.y());
      q.setZ(q_w_curr.z());
      transform.setRotation(q);
      br.sendTransform(tf::StampedTransform(transform,
                                            odomAftMapped.header.stamp,
                                            "/camera_init", "/aft_mapped"));

      frameCount++;
    }
    std::chrono::milliseconds dura(2);
    std::this_thread::sleep_for(dura);
  }
}

int main(int argc, char **argv) {

  ros::init(argc, argv, "laserMapping");
  ros::NodeHandle nh;

  float lineRes = 0;
  float planeRes = 0;
  std::string pcd_save_path;
  nh.param<float>("mapping_line_resolution", lineRes, 0.4);
  nh.param<float>("mapping_plane_resolution", planeRes, 0.8);
  nh.param<std::string>("pcd_save_path",pcd_save_path,"/home/admin/workspace/src/PCD/PCD.pcd");
  ROS_INFO("line resolution %f plane resolution %f \n", lineRes, planeRes);
  downSizeFilterCorner.setLeafSize(lineRes, lineRes, lineRes);
  downSizeFilterSurf.setLeafSize(planeRes, planeRes, planeRes);

  ros::Subscriber subCamera = nh.subscribe<sensor_msgs::Image>(
      "/image_topic", 100, cameraHandler);


  ros::Subscriber subLaserCloudCornerLast =
      nh.subscribe<sensor_msgs::PointCloud2>("/laser_cloud_corner_last", 100,
                                             laserCloudCornerLastHandler);

  ros::Subscriber subLaserCloudSurfLast =
      nh.subscribe<sensor_msgs::PointCloud2>("/laser_cloud_surf_last", 100,
                                             laserCloudSurfLastHandler);

  ros::Subscriber subLaserOdometry = nh.subscribe<nav_msgs::Odometry>(
      "/laser_odom_to_init", 100, laserOdometryHandler);

  ros::Subscriber subLaserCloudFullRes = nh.subscribe<sensor_msgs::PointCloud2>(
      "/velodyne_cloud_3", 100, laserCloudFullResHandler);

  pubLaserCloudSurround =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_surround", 100);

  pubLaserCloudMap =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_map", 100);

  pubLaserCloudFullRes =
      nh.advertise<sensor_msgs::PointCloud2>("/velodyne_cloud_registered", 100);

  pubLaserColor =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_rgb", 100);


  pubOdomAftMapped =
      nh.advertise<nav_msgs::Odometry>("/aft_mapped_to_init", 100);

  pubOdomAftMappedHighFrec =
      nh.advertise<nav_msgs::Odometry>("/aft_mapped_to_init_high_frec", 100);

  pubLaserAfterMappedPath =
      nh.advertise<nav_msgs::Path>("/aft_mapped_path", 100);


  nh.param<vector<double>>("mapping/extrinsic_T", extrinT, vector<double>());
  nh.param<vector<double>>("mapping/extrinsic_R", extrinR, vector<double>());
  // color mapping param
  nh.param<vector<double>>("color_mapping/extrinsic_T", extrinT_lc, vector<double>());
  nh.param<vector<double>>("color_mapping/extrinsic_R", extrinR_lc, vector<double>());
  nh.param<vector<double>>("color_mapping/K_camera", K_camera, vector<double>());
  nh.param<vector<double>>("color_mapping/D_camera", D_camera, vector<double>());
  nh.param<bool>("use_color", use_color, true);  

  for (int i = 0; i < laserCloudNum; i++) {
    laserCloudCornerArray[i].reset(new pcl::PointCloud<PointType>());
    laserCloudSurfArray[i].reset(new pcl::PointCloud<PointType>());
  }


  Lidar_T_wrt_IMU<<VEC_FROM_ARRAY(extrinT);
  Lidar_R_wrt_IMU<<MAT_FROM_ARRAY(extrinR);
  Camera_T_wrt_Lidar<<VEC_FROM_ARRAY(extrinT_lc);
  Camera_R_wrt_Lidar<<MAT_FROM_ARRAY(extrinR_lc);
  Eigen::Isometry3d T_IL = Eigen::Isometry3d::Identity();
  Eigen::Isometry3d T_LC = Eigen::Isometry3d::Identity();
  T_IL.translate(Lidar_T_wrt_IMU);
  T_IL.rotate(Lidar_R_wrt_IMU);
  T_LC.translate(Camera_T_wrt_Lidar);
  T_LC.rotate(Camera_R_wrt_Lidar);


  std::thread mapping_process{process};

  ros::spin();

  // pcl::PCDWriter pcd_writer;
  // pcd_writer.writeBinary(pcd_save_path, *laserCloudWaitSave);
  // pcd_writer.writeBinary("/home/gabriel/loam_horizon_ws/src/livox_horizon_loam/PCD/color_map.pcd", *laserColorCloudWaitSave);
  
  
  if (use_color){
    pclRGBToLaszip(laserColorCloudWaitSave, pcd_save_path);
  }
  else{
    pclToLaszip(laserCloudWaitSave, pcd_save_path);
  }
  
  return 0;
}

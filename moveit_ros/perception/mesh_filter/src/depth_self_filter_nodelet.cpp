/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Suat Gedikli */

#include <moveit/mesh_filter/depth_self_filter_nodelet.h>
#include <moveit/mesh_filter/stereo_camera_model.h>
#include <moveit/mesh_filter/mesh_filter.h>
#include <ros/ros.h>
#include <image_transport/subscriber_filter.h>
#include <sensor_msgs/image_encodings.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_model/robot_model.h>
#include <cv_bridge/cv_bridge.h>
#include <geometric_shapes/mesh_operations.h>
#include <geometric_shapes/shapes.h>

namespace enc = sensor_msgs::image_encodings;
static const std::string LOGNAME = "depth_self_filter_nodelet";

mesh_filter::DepthSelfFiltering::~DepthSelfFiltering()
{
}

void mesh_filter::DepthSelfFiltering::onInit()
{
  ros::NodeHandle& nh = getNodeHandle();
  ros::NodeHandle& private_nh = getPrivateNodeHandle();
  input_depth_transport_ = std::make_shared<image_transport::ImageTransport>(nh);
  filtered_depth_transport_ = std::make_shared<image_transport::ImageTransport>(nh);
  filtered_label_transport_ = std::make_shared<image_transport::ImageTransport>(nh);
  model_depth_transport_ = std::make_shared<image_transport::ImageTransport>(nh);
  model_label_transport_ = std::make_shared<image_transport::ImageTransport>(nh);

  // Read parameters
  private_nh.param("queue_size", queue_size_, 1);
  private_nh.param("near_clipping_plane_distance", near_clipping_plane_distance_, 0.4);
  private_nh.param("far_clipping_plane_distance", far_clipping_plane_distance_, 5.0);
  ;
  private_nh.param("shadow_threshold", shadow_threshold_, 0.3);
  private_nh.param("padding_scale", padding_scale_, 1.0);
  private_nh.param("padding_offset", padding_offset_, 0.005);
  double tf_update_rate = 30.;
  private_nh.param("tf_update_rate", tf_update_rate, 30.0);
  transform_provider_.setUpdateRate(tf_update_rate);

  image_transport::SubscriberStatusCallback itssc = [this](auto&& /*unused*/) { connectCb(); };
  ros::SubscriberStatusCallback rssc = [this](auto&& /*unused*/) { connectCb(); };

  std::lock_guard<std::mutex> lock(connect_mutex_);
  pub_filtered_depth_image_ =
      filtered_depth_transport_->advertiseCamera("/filtered/depth", queue_size_, itssc, itssc, rssc, rssc);
  pub_filtered_label_image_ =
      filtered_label_transport_->advertiseCamera("/filtered/labels", queue_size_, itssc, itssc, rssc, rssc);
  pub_model_depth_image_ =
      model_depth_transport_->advertiseCamera("/model/depth", queue_size_, itssc, itssc, rssc, rssc);
  pub_model_label_image_ =
      model_depth_transport_->advertiseCamera("/model/label", queue_size_, itssc, itssc, rssc, rssc);

  filtered_depth_ptr_ = std::make_shared<cv_bridge::CvImage>();
  filtered_label_ptr_ = std::make_shared<cv_bridge::CvImage>();
  model_depth_ptr_ = std::make_shared<cv_bridge::CvImage>();
  model_label_ptr_ = std::make_shared<cv_bridge::CvImage>();

  mesh_filter_ = std::make_shared<MeshFilter<StereoCameraModel>>(
      [&tfp = transform_provider_](mesh_filter::MeshHandle mesh, Eigen::Isometry3d& tf) {
        return tfp.getTransform(mesh, tf);
      },
      mesh_filter::StereoCameraModel::REGISTERED_PSDK_PARAMS);
  mesh_filter_->parameters().setDepthRange(near_clipping_plane_distance_, far_clipping_plane_distance_);
  mesh_filter_->setShadowThreshold(shadow_threshold_);
  mesh_filter_->setPaddingOffset(padding_offset_);
  mesh_filter_->setPaddingScale(padding_scale_);
  // add meshesfla
  addMeshes(*mesh_filter_);
  transform_provider_.start();
}

void mesh_filter::DepthSelfFiltering::filter(const sensor_msgs::ImageConstPtr& depth_msg,
                                             const sensor_msgs::CameraInfoConstPtr& info_msg)
{
  transform_provider_.setFrame(depth_msg->header.frame_id);
  // do filtering here
  mesh_filter::StereoCameraModel::Parameters& params = mesh_filter_->parameters();
  params.setCameraParameters(info_msg->K[0], info_msg->K[4], info_msg->K[2], info_msg->K[5]);
  params.setImageSize(depth_msg->width, depth_msg->height);

  // Handling of two possible encodings of a depth image: 16UC1 and 32FC1
  if (depth_msg->encoding == sensor_msgs::image_encodings::TYPE_16UC1)
  {
    ROS_WARN_STREAM_ONCE_NAMED(LOGNAME,
                               "The input depth image uses a 16UC1 encoding. Consider converting the publisher to "
                               "generate the canonical 32FC1 encoding instead according to ROS REP-118.");
    mesh_filter_->filter(depth_msg->data.data(), GL_UNSIGNED_SHORT);
  }
  else if (depth_msg->encoding == sensor_msgs::image_encodings::TYPE_32FC1)
    mesh_filter_->filter(reinterpret_cast<const float*>(depth_msg->data.data()), GL_FLOAT);
  else
  {
    ROS_WARN_STREAM_THROTTLE_NAMED(1.0, LOGNAME, "Unsupported encoding of depth image: " << depth_msg->encoding);
    return;
  }

  if (pub_filtered_depth_image_.getNumSubscribers() > 0)
  {
    filtered_depth_ptr_->encoding = sensor_msgs::image_encodings::TYPE_32FC1;
    filtered_depth_ptr_->header = depth_msg->header;

    if (static_cast<uint32_t>(filtered_depth_ptr_->image.cols) != depth_msg->width ||
        static_cast<uint32_t>(filtered_depth_ptr_->image.rows) != depth_msg->height)
      filtered_depth_ptr_->image = cv::Mat(depth_msg->height, depth_msg->width, CV_32FC1);
    mesh_filter_->getFilteredDepth((float*)filtered_depth_ptr_->image.data);
    pub_filtered_depth_image_.publish(filtered_depth_ptr_->toImageMsg(), info_msg);
  }

  // this is from rendering of the model
  if (pub_model_depth_image_.getNumSubscribers() > 0)
  {
    model_depth_ptr_->encoding = sensor_msgs::image_encodings::TYPE_32FC1;
    model_depth_ptr_->header = depth_msg->header;

    if (static_cast<uint32_t>(model_depth_ptr_->image.cols) != depth_msg->width ||
        static_cast<uint32_t>(model_depth_ptr_->image.rows) != depth_msg->height)
      model_depth_ptr_->image = cv::Mat(depth_msg->height, depth_msg->width, CV_32FC1);
    mesh_filter_->getModelDepth((float*)model_depth_ptr_->image.data);
    pub_model_depth_image_.publish(model_depth_ptr_->toImageMsg(), info_msg);
  }

  if (pub_filtered_label_image_.getNumSubscribers() > 0)
  {
    filtered_label_ptr_->encoding = sensor_msgs::image_encodings::RGBA8;
    filtered_label_ptr_->header = depth_msg->header;

    if (static_cast<uint32_t>(filtered_label_ptr_->image.cols) != depth_msg->width ||
        static_cast<uint32_t>(filtered_label_ptr_->image.rows) != depth_msg->height)
      filtered_label_ptr_->image = cv::Mat(depth_msg->height, depth_msg->width, CV_8UC4);
    mesh_filter_->getFilteredLabels((unsigned int*)filtered_label_ptr_->image.data);
    pub_filtered_label_image_.publish(filtered_label_ptr_->toImageMsg(), info_msg);
  }

  if (pub_model_label_image_.getNumSubscribers() > 0)
  {
    model_label_ptr_->encoding = sensor_msgs::image_encodings::RGBA8;
    model_label_ptr_->header = depth_msg->header;
    if (static_cast<uint32_t>(model_label_ptr_->image.cols) != depth_msg->width ||
        static_cast<uint32_t>(model_label_ptr_->image.rows) != depth_msg->height)
      model_label_ptr_->image = cv::Mat(depth_msg->height, depth_msg->width, CV_8UC4);
    mesh_filter_->getModelLabels((unsigned int*)model_label_ptr_->image.data);
    pub_model_label_image_.publish(model_label_ptr_->toImageMsg(), info_msg);
  }
}

void mesh_filter::DepthSelfFiltering::addMeshes(MeshFilter<StereoCameraModel>& mesh_filter)
{
  robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
  moveit::core::RobotModelConstPtr robot_model = robot_model_loader.getModel();
  const auto& links = robot_model->getLinkModelsWithCollisionGeometry();
  for (auto link : links)
  {
    const auto& shapes = link->getShapes();
    for (const auto& shape : shapes)
    {
      if (shape->type == shapes::MESH)
      {
        const shapes::Mesh& m = static_cast<const shapes::Mesh&>(*shape);
        MeshHandle mesh_handle = mesh_filter.addMesh(m);
        transform_provider_.addHandle(mesh_handle, link->getName());
      }

      else if (shape->type == shapes::BOX)
      {
        shapes::Mesh* mesh = shapes::createMeshFromShape(static_cast<const shapes::Box&>(*shape));
        MeshHandle mesh_handle = mesh_filter.addMesh(*mesh);
        transform_provider_.addHandle(mesh_handle, link->getName());
      }

      else if (shape->type == shapes::CONE)
      {
        shapes::Mesh* mesh = shapes::createMeshFromShape(static_cast<const shapes::Cone&>(*shape));
        MeshHandle mesh_handle = mesh_filter.addMesh(*mesh);
        transform_provider_.addHandle(mesh_handle, link->getName());
      }

      else if (shape->type == shapes::CYLINDER)
      {
        shapes::Mesh* mesh = shapes::createMeshFromShape(static_cast<const shapes::Cylinder&>(*shape));
        MeshHandle mesh_handle = mesh_filter.addMesh(*mesh);
        transform_provider_.addHandle(mesh_handle, link->getName());
      }

      else if (shape->type == shapes::SPHERE)
      {
        shapes::Mesh* mesh = shapes::createMeshFromShape(static_cast<const shapes::Sphere&>(*shape));
        MeshHandle mesh_handle = mesh_filter.addMesh(*mesh);
        transform_provider_.addHandle(mesh_handle, link->getName());
      }

      else
      {
        ROS_WARN_STREAM_NAMED(LOGNAME, "Shape " << shape->type << " is not supported!");
      }
    }
  }
}

// Handles (un)subscribing when clients (un)subscribe
void mesh_filter::DepthSelfFiltering::connectCb()
{
  std::lock_guard<std::mutex> lock(connect_mutex_);
  if (pub_filtered_depth_image_.getNumSubscribers() == 0 && pub_filtered_label_image_.getNumSubscribers() == 0 &&
      pub_model_depth_image_.getNumSubscribers() == 0 && pub_model_label_image_.getNumSubscribers() == 0)
  {
    sub_depth_image_.shutdown();
  }
  else if (!sub_depth_image_)
  {
    image_transport::TransportHints hints("raw", ros::TransportHints(), getPrivateNodeHandle());
    sub_depth_image_ =
        input_depth_transport_->subscribeCamera("depth", queue_size_, &DepthSelfFiltering::depthCb, this, hints);
  }
}

void mesh_filter::DepthSelfFiltering::depthCb(const sensor_msgs::ImageConstPtr& depth_msg,
                                              const sensor_msgs::CameraInfoConstPtr& info_msg)
{
  filter(depth_msg, info_msg);
}

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(mesh_filter::DepthSelfFiltering, nodelet::Nodelet)

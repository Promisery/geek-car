/******************************************************************************
 * MIT License

 * Copyright (c) 2019 Geekstyle

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
******************************************************************************/
#include "modules/sensors/realsense_component.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include "librealsense2/rs.hpp"
#include "opencv2/calib3d.hpp"
#include "opencv2/opencv.hpp"

#include "cyber/common/log.h"
#include "cyber/cyber.h"
#include "cyber/proto/qos_profile.pb.h"
#include "cyber/proto/role_attributes.pb.h"
#include "cyber/time/rate.h"
#include "cyber/time/time.h"
#include "modules/sensors/proto/sensors.pb.h"
#include "modules/sensors/realsense.h"

namespace apollo {
namespace sensors {

using apollo::cyber::Rate;
using apollo::cyber::Time;
using apollo::cyber::proto::QosDurabilityPolicy;
using apollo::cyber::proto::QosHistoryPolicy;
using apollo::cyber::proto::QosReliabilityPolicy;
using apollo::cyber::proto::RoleAttributes;
using apollo::sensors::Acc;
using apollo::sensors::Gyro;
using apollo::sensors::Image;
using apollo::sensors::Pose;

rs2::device get_device(const std::string& serial_number = "") {
  rs2::context ctx;
  while (true) {
    for (auto&& dev : ctx.query_devices()) {
      if (((serial_number.empty() &&
            std::strstr(dev.get_info(RS2_CAMERA_INFO_NAME),
                        FLAGS_device_model.c_str())) ||
           std::strcmp(dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER),
                       serial_number.c_str()) == 0))
        return dev;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/**
 * @brief device and sensor init
 *
 * @return true
 * @return false
 */
bool RealsenseComponent::Init() {
  device_ = get_device(FLAGS_serial_number);

  // print device information
  RealSense::printDeviceInformation(device_);

  AINFO << "Got device with serial number "
        << device_.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
  cyber::SleepFor(std::chrono::milliseconds(device_wait_));

  sensor_ = device_.first<rs2::sensor>();
  // print sensor option to log
  RealSense::getSensorOption(sensor_);
  sensor_.set_option(RS2_OPTION_FRAMES_QUEUE_SIZE, 0);
  sensor_.open(sensor_.get_stream_profiles());

  Calibration();

  // use quality of service to up pose channel reliability
  RoleAttributes pose_attr;
  pose_attr.set_channel_name(FLAGS_pose_channel);
  auto qos = pose_attr.mutable_qos_profile();
  qos->set_history(QosHistoryPolicy::HISTORY_KEEP_LAST);
  qos->set_depth(10);
  qos->set_mps(30);
  qos->set_reliability(QosReliabilityPolicy::RELIABILITY_RELIABLE);
  qos->set_durability(QosDurabilityPolicy::DURABILITY_VOLATILE);

  pose_writer_ = node_->CreateWriter<Pose>(pose_attr);

  // use quality of service to up raw image channel reliability
  RoleAttributes image_attr;
  image_attr.set_channel_name(FLAGS_raw_image_channel);
  auto qos = image_attr.mutable_qos_profile();
  qos->set_history(QosHistoryPolicy::HISTORY_KEEP_LAST);
  qos->set_depth(10);
  qos->set_mps(30);
  qos->set_reliability(QosReliabilityPolicy::RELIABILITY_RELIABLE);
  qos->set_durability(QosDurabilityPolicy::DURABILITY_VOLATILE);

  image_writer_ = node_->CreateWriter<Image>(image_attr);

  if (FLAGS_publish_acc) {
    acc_writer_ = node_->CreateWriter<Acc>(FLAGS_acc_channel);
  }

  if (FLAGS_publish_gyro) {
    gyro_writer_ = node_->CreateWriter<Gyro>(FLAGS_gyro_channel);
  }

  sensor_.start([this](rs2::frame f) {
    q_.enqueue(std::move(f));  // enqueue any new frames into q
  });

  async_result_ = cyber::Async(&RealsenseComponent::run, this);
  return true;
}

/**
 * [RealSense:: collect frames of T265]
 * @return [description]
 */
void RealsenseComponent::run() {
  int times = 1;
  while (!cyber::IsShutdown()) {
    std::cout << times << std::endl;
    // wait for device is ready. in case of device is busy
    if (!device_) {
      device_ = get_device();
      continue;
    }

    // wait until new frame is available and dequeue it
    // handle frames in the main event loop
    rs2::frame f = q_.wait_for_frame();

    // core of data write to channel
    if (f.get_profile().stream_type() == RS2_STREAM_POSE &&
        FLAGS_publish_pose) {
      auto pose_frame = f.as<rs2::pose_frame>();
      auto pose_data = pose_frame.get_pose_data();
      AINFO << "pose " << pose_data.translation;
      if (pose_frame.get_frame_number() % 5 == 0) {
        OnPose(pose_data, pose_frame.get_frame_number());
      }
    } else if (f.get_profile().stream_type() == RS2_STREAM_GYRO &&
               FLAGS_publish_gyro) {
      auto gyro_frame = f.as<rs2::motion_frame>();
      rs2_vector gyro = gyro_frame.get_motion_data();
      AINFO << "Gyro:" << gyro.x << ", " << gyro.y << ", " << gyro.z;
      OnGyro(gyro, gyro_frame.get_frame_number());
    } else if (f.get_profile().stream_type() == RS2_STREAM_ACCEL &&
               FLAGS_publish_acc) {
      auto accel_frame = f.as<rs2::motion_frame>();
      rs2_vector accel = accel_frame.get_motion_data();
      AINFO << "Accel:" << accel.x << ", " << accel.y << ", " << accel.z;
      OnAcc(accel, accel_frame.get_frame_number());
    } else if (f.get_profile().stream_type() == RS2_STREAM_FISHEYE &&
               f.get_profile().stream_index() == 1 && FLAGS_publish_raw_image) {
      // left fisheye
      auto fisheye_frame = f.as<rs2::video_frame>();

      AINFO << "fisheye " << f.get_profile().stream_index() << ", "
            << fisheye_frame.get_width() << "x" << fisheye_frame.get_height();

      cv::Mat image(
          cv::Size(fisheye_frame.get_width(), fisheye_frame.get_height()),
          CV_8U, (void*)fisheye_frame.get_data(), cv::Mat::AUTO_STEP);
      cv::Mat dst;
      cv::remap(image, dst, map1_, map2_, cv::INTER_LINEAR);
      cv::Size dsize = cv::Size(static_cast<int>(dst.cols * 0.5),
                                static_cast<int>(dst.rows * 0.5));
      cv::Mat new_size_img(dsize, CV_8U);
      cv::resize(dst, new_size_img, dsize);
      if (fisheye_frame.get_frame_number() % 2 == 0) {
        OnImage(new_size_img, fisheye_frame.get_frame_number());
      }
    }
    times++;
  }
}

/**
 * @brief fisheye calibration
 *
 */
void RealsenseComponent::Calibration() {
  cv::Mat intrinsicsL;
  cv::Mat distCoeffsL;
  rs2_intrinsics left = sensor_.get_stream_profiles()[0]
                            .as<rs2::video_stream_profile>()
                            .get_intrinsics();
  intrinsicsL = (cv::Mat_<double>(3, 3) << left.fx, 0, left.ppx, 0, left.fy,
                 left.ppy, 0, 0, 1);
  distCoeffsL = cv::Mat(1, 4, CV_32F, left.coeffs);
  cv::Mat R = cv::Mat::eye(3, 3, CV_32F);
  cv::Mat P = (cv::Mat_<double>(3, 4) << left.fx, 0, left.ppx, 0, 0, left.fy,
               left.ppy, 0, 0, 0, 1, 0);

  cv::fisheye::initUndistortRectifyMap(intrinsicsL, distCoeffsL, R, P,
                                       cv::Size(848, 816), CV_16SC2, map1_,
                                       map2_);
}

/**
 * @brief callback of Image data
 *
 * @param dst
 * @return true
 * @return false
 */
void RealsenseComponent::OnImage(cv::Mat dst, uint64 frame_no) {
  auto image_proto = std::make_shared<Image>();
  image_proto->set_frame_no(frame_no);
  image_proto->set_height(dst.rows);
  image_proto->set_width(dst.cols);
  image_proto->set_encoding(rs2_format_to_string(RS2_FORMAT_Y8));
  image_proto->set_measurement_time(Time::Now().ToSecond());
  auto m_size = dst.rows * dst.cols * dst.elemSize();
  image_proto->set_data(dst.data, m_size);
  image_writer_->Write(image_proto);
}

/**
 * @brief callback of Pose data
 *
 * @param pose_data
 * @return true
 * @return false
 */
void RealsenseComponent::OnPose(rs2_pose pose_data, uint64 frame_no) {
  auto pose_proto = std::make_shared<Pose>();
  pose_proto->set_frame_no(frame_no);
  pose_proto->set_tracker_confidence(pose_data.tracker_confidence);
  pose_proto->set_mapper_confidence(pose_data.mapper_confidence);

  auto translation = pose_proto->mutable_translation();
  translation->set_x(pose_data.translation.x);
  translation->set_y(pose_data.translation.y);
  translation->set_z(pose_data.translation.z);

  auto velocity = pose_proto->mutable_velocity();
  velocity->set_x(pose_data.velocity.x);
  velocity->set_y(pose_data.velocity.y);
  velocity->set_z(pose_data.velocity.z);

  auto rotation = pose_proto->mutable_rotation();
  rotation->set_x(pose_data.rotation.x);
  rotation->set_y(pose_data.rotation.y);
  rotation->set_z(pose_data.rotation.z);

  auto angular_velocity = pose_proto->mutable_angular_velocity();
  angular_velocity->set_x(pose_data.angular_velocity.x);
  angular_velocity->set_y(pose_data.angular_velocity.y);
  angular_velocity->set_z(pose_data.angular_velocity.z);

  pose_writer_->Write(pose_proto);
}

void RealsenseComponent::OnAcc(rs2_vector acc, uint64 frame_no) {
  auto proto_accel = std::make_shared<Acc>();
  proto_accel->mutable_acc()->set_x(acc.x);
  proto_accel->mutable_acc()->set_y(acc.y);
  proto_accel->mutable_acc()->set_z(acc.z);

  acc_writer_->Write(proto_accel);
}

void RealsenseComponent::OnGyro(rs2_vector gyro, uint64 frame_no) {
  auto proto_gyro = std::make_shared<Gyro>();
  proto_gyro->mutable_gyro()->set_x(gyro.x);
  proto_gyro->mutable_gyro()->set_y(gyro.y);
  proto_gyro->mutable_gyro()->set_z(gyro.z);

  gyro_writer_->Write(proto_gyro);
}

RealsenseComponent::~RealsenseComponent() {
  sensor_.stop();
  sensor_.close();
  async_result_.wait();
}

}  // namespace sensors
}  // namespace apollo

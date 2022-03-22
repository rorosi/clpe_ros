#pragma once

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>

#include "errors.hpp"

namespace clpe
{
enum class CalibrationModel : uint32_t {
  Jhang = 0,
  FishEye = 1,
};

// TODO: docs say 95 bytes, but reference sheet is 107 bytes
struct __attribute__((packed)) EepromData {
  uint16_t signature_code;
  uint64_t version;
  CalibrationModel calibration_model;
  float fx;
  float fy;
  float cx;
  float cy;
  float k1;
  float k2;
  float k3;
  float k4;
  float rms;
  float fov;
  float calibration_temperature;
  uint8_t reserved1[20];
  float p1;
  float p2;
  uint8_t reserved2[8];
  uint16_t checksum;
  uint8_t production_date[11];
};

template <typename ClpeClientApi>
class ClpeNode : public rclcpp::Node
{
public:
  ClpeClientApi clpe_api;

  ClpeNode(ClpeClientApi && clpe_api) : rclcpp::Node("clpe"), clpe_api(std::move(clpe_api))
  {
    // declare ros params
    {
      rcl_interfaces::msg::ParameterDescriptor desc;
      desc.description = "sudo password";
      desc.read_only = true;
      this->declare_parameter("password", rclcpp::ParameterValue(), desc);
    }

    //
    {
    }
  }

  /**
   * Initialize ClpeClient
   */
  std::error_code Init()
  {
    // FIXME: This requires sudo password!!
    const auto & password = this->get_parameter("password").get_value<std::string>();
    const auto result = this->clpe_api.Clpe_Connection(password);
    if (result != 0) {
      return std::error_code(result, ConnectionError::get());
    }
    return kNoError;
  }

  /**
   * Reading the camera's eeprom is slow so callers should cache the result
   */
  std::error_code GetCameraInfo(int cam_id, sensor_msgs::msg::CameraInfo & cam_info)
  {
    // reset to defaults
    cam_info = sensor_msgs::msg::CameraInfo();
    // calibration may change anytime for self calibrating systems, so we cannot cache the cam info.
    cam_info.width = 1920;
    cam_info.height = 1080;
    EepromData eeprom_data;
    const auto result =
        this->clpe_api.Clpe_GetEepromData(cam_id, reinterpret_cast<unsigned char *>(&eeprom_data));
    if (result != 0) {
      return std::error_code(result, GetEepromDataError::get());
    }
    switch (eeprom_data.calibration_model) {
      case CalibrationModel::Jhang:
        cam_info.distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;
        break;
      case CalibrationModel::FishEye:
        cam_info.distortion_model = sensor_msgs::distortion_models::EQUIDISTANT;
        break;
    }
    cam_info.k = {eeprom_data.fx, 0, eeprom_data.cx, 0, eeprom_data.fy, eeprom_data.cy, 0, 0, 1};
    cam_info.d = {eeprom_data.k1, eeprom_data.k2, eeprom_data.p1,
                  eeprom_data.p2, eeprom_data.k3, eeprom_data.k4};
    return kNoError;
  }

  static void FillImageMsg(unsigned char * buffer, unsigned int size, const timeval & timestamp,
                           sensor_msgs::msg::Image & image)
  {
    image.header.frame_id = "base_link";
    image.header.stamp.sec = timestamp.tv_sec;
    image.header.stamp.nanosec = timestamp.tv_usec * 1000;
    // buffer is only valid for 16 frames, since ros2 publish has no real time guarantees, we must
    // copy the data out to avoid UB.
    image.data.reserve(size);
    std::copy(buffer, buffer + size, image.data.data());
    image.encoding = sensor_msgs::image_encodings::YUV422;
    image.width = 1920;
    image.height = 1080;
    // assume that each row is same sized.
    image.step = size / 1080;
    image.is_bigendian = false;
  }

  std::error_code GetCameraImage(int cam_id, sensor_msgs::msg::Image & image)
  {
    unsigned char * buffer;
    unsigned int size;
    timeval timestamp;
    const auto result = this->clpe_api.Clpe_GetFrameOneCam(cam_id, &buffer, &size, &timestamp);
    if (result != 0) {
      return std::error_code(result, GetFrameError::get());
    }
    this->FillImageMsg(buffer, size, timestamp, image);
    return kNoError;
  }
};
}  // namespace clpe

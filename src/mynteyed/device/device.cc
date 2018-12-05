// Copyright 2018 Slightech Co., Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "mynteyed/device/device.h"
#include <cstring>
#include <fstream>
#include <string>

#include "mynteyed/util/log.h"

MYNTEYE_USE_NAMESPACE

namespace {

void get_stream_size(const StreamMode& stream_mode, int* width, int* height) {
  switch (stream_mode) {
    case StreamMode::STREAM_640x480:
      *width = 640;
      *height = 480;
      break;
    case StreamMode::STREAM_1280x480:
      *width = 1280;
      *height = 480;
      break;
    case StreamMode::STREAM_1280x720:
      *width = 1280;
      *height = 720;
      break;
    case StreamMode::STREAM_2560x720:
      *width = 2560;
      *height = 720;
      break;
    default:
      throw new std::runtime_error("StreamMode is unknown");
  }
}

std::string get_stream_format_string(const StreamFormat& stream_format) {
  switch (stream_format) {
    case StreamFormat::STREAM_MJPG:
      return "MJPG";
    case StreamFormat::STREAM_YUYV:
      return "YUYV";
    default:
      throw new std::runtime_error("StreamFormat is unknown");
  }
}

}  // namespace

Device::Device()
  : etron_di_(nullptr), dev_sel_info_({-1}),
    camera_calibrations_({nullptr, nullptr}) {
  DBG_LOGD(__func__);
  Init();
}

Device::~Device() {
  DBG_LOGD(__func__);
  free(stream_color_info_ptr_);
  free(stream_depth_info_ptr_);
  Close();
}

void Device::Init() {
  int ret = EtronDI_Init(&etron_di_, false);
  DBG_LOGI("MYNTEYE Init: %d", ret);
  UNUSED(ret);

  stream_color_info_ptr_ =
      (PETRONDI_STREAM_INFO)malloc(sizeof(ETRONDI_STREAM_INFO)*64);
  stream_depth_info_ptr_ =
      (PETRONDI_STREAM_INFO)malloc(sizeof(ETRONDI_STREAM_INFO)*64);
  // default image type
  depth_data_type_ = 2;
  // default frame rate
  framerate_ = 10;

  color_device_opened_ = false;
  depth_device_opened_ = false;

  ir_depth_only_enabled_ = false;
  color_ir_depth_only_enabled_ = false;
  depth_ir_depth_only_enabled_ = false;

  OnInit();
}

void Device::GetDeviceInfos(std::vector<DeviceInfo>* dev_infos) {
  if (!dev_infos) {
    LOGE("GetDevices: dev_infos is null.");
    return;
  }
  dev_infos->clear();

  int count = EtronDI_GetDeviceNumber(etron_di_);
  DBG_LOGD("GetDevices: %d", count);

  DEVSELINFO dev_sel_info;
  DEVINFORMATION* p_dev_info =
      (DEVINFORMATION*)malloc(sizeof(DEVINFORMATION)*count);  // NOLINT

  for (int i = 0; i < count; i++) {
    dev_sel_info.index = i;

    EtronDI_GetDeviceInfo(etron_di_, &dev_sel_info, p_dev_info+i);

    char sz_buf[256];
    int actual_length = 0;
    if (ETronDI_OK == EtronDI_GetFwVersion(
        etron_di_, &dev_sel_info, sz_buf, 256, &actual_length)) {
      DeviceInfo info;
      info.index = i;
      info.name = p_dev_info[i].strDevName;
      info.type = p_dev_info[i].nDevType;
      info.pid = p_dev_info[i].wPID;
      info.vid = p_dev_info[i].wVID;
      info.chip_id = p_dev_info[i].nChipID;
      info.fw_version = sz_buf;
      dev_infos->push_back(std::move(info));
    }
  }

  free(p_dev_info);
}

void Device::GetStreamInfos(const std::int32_t& dev_index,
    std::vector<StreamInfo>* color_infos,
    std::vector<StreamInfo>* depth_infos) {
  if (!color_infos) {
    LOGE("GetResolutions: color_infos is null.");
    return;
  }
  color_infos->clear();

  if (!depth_infos) {
    LOGE("GetResolutions: depth_infos is null.");
    return;
  }
  depth_infos->clear();

  memset(stream_color_info_ptr_, 0, sizeof(ETRONDI_STREAM_INFO)*64);
  memset(stream_depth_info_ptr_, 0, sizeof(ETRONDI_STREAM_INFO)*64);

  DEVSELINFO dev_sel_info{dev_index};
  EtronDI_GetDeviceResolutionList(etron_di_, &dev_sel_info, 64,
      stream_color_info_ptr_, 64, stream_depth_info_ptr_);

  PETRONDI_STREAM_INFO stream_temp_info_ptr = stream_color_info_ptr_;
  int i = 0;
  while (i < 64) {
    if (stream_temp_info_ptr->nWidth > 0) {
      StreamInfo info;
      info.index = i;
      info.width = stream_temp_info_ptr->nWidth;
      info.height = stream_temp_info_ptr->nHeight;
      info.format = stream_temp_info_ptr->bFormatMJPG ?
          StreamFormat::STREAM_MJPG : StreamFormat::STREAM_YUYV;
      color_infos->push_back(info);
    }
    stream_temp_info_ptr++;
    i++;
  }

  stream_temp_info_ptr = stream_depth_info_ptr_;
  i = 0;
  while (i < 64) {
    if (stream_temp_info_ptr->nWidth > 0) {
      StreamInfo info;
      info.index = i;
      info.width = stream_temp_info_ptr->nWidth;
      info.height = stream_temp_info_ptr->nHeight;
      info.format = stream_temp_info_ptr->bFormatMJPG ?
          StreamFormat::STREAM_MJPG : StreamFormat::STREAM_YUYV;
      depth_infos->push_back(info);
    }
    stream_temp_info_ptr++;
    i++;
  }

  stream_info_dev_index_ = dev_index;
}

bool Device::SetAutoExposureEnabled(bool enabled) {
  bool ok;
  if (enabled) {
    ok = ETronDI_OK == EtronDI_EnableAE(etron_di_, &dev_sel_info_);
  } else {
    ok = ETronDI_OK == EtronDI_DisableAE(etron_di_, &dev_sel_info_);
  }
  if (ok) {
    LOGI("-- Auto-exposure state: %s", enabled ? "enabled" : "disabled");
  } else {
    LOGW("-- %s auto-exposure failed", enabled ? "Enable" : "Disable");
  }
  return ok;
}

bool Device::SetAutoWhiteBalanceEnabled(bool enabled) {
  bool ok;
  if (enabled) {
    ok = ETronDI_OK == EtronDI_EnableAWB(etron_di_, &dev_sel_info_);
  } else {
    ok = ETronDI_OK == EtronDI_DisableAWB(etron_di_, &dev_sel_info_);
  }
  if (ok) {
    LOGI("-- Auto-white balance state: %s", enabled ? "enabled" : "disabled");
  } else {
    LOGW("-- %s auto-white balance failed", enabled ? "Enable" : "Disable");
  }
  return ok;
}

void Device::SetInfraredDepthOnly(const OpenParams& params) {
  if (!params.ir_depth_only) {
    EtronDI_EnableInterleave(etron_di_, &dev_sel_info_, false);
    return;
  }

  int error_n = 0;
  if (params.dev_mode != DeviceMode::DEVICE_ALL) {
    error_n = 1;
    EtronDI_EnableInterleave(etron_di_, &dev_sel_info_, false);
  } else if (params.framerate < 15 || params.framerate > 30) {
    error_n = 2;
    EtronDI_EnableInterleave(etron_di_, &dev_sel_info_, false);
  } else if (params.stream_mode == StreamMode::STREAM_2560x720 &&
      params.framerate > 15) {
    error_n = 3;
    EtronDI_EnableInterleave(etron_di_, &dev_sel_info_, false);
  }

  if (error_n > 0) {
    throw_error("\n\nNote: IR Depth Only mode support frame"
        " rate only be between 15fps and 30fps.\n"
        "    When dev_mode != DeviceMode::DEVICE_ALL,"
        " IR Depth Only mode not be supported.\n"
        "    When stream_mode == StreamMode::STREAM_2560x720,"
        " frame rate only be 15fps in this mode.\n"
        "    When frame rate less than 15fps or greater than 30fps,"
        " IR Depth Only mode will be not available.\n");

    return;
  }

  if (depth_data_type_ == 1 ||
      depth_data_type_ == 2 ||
      depth_data_type_ == 4) {
    color_ir_depth_only_enabled_ = false;
    depth_ir_depth_only_enabled_ = true;
  } else {
    color_ir_depth_only_enabled_ = true;
    depth_ir_depth_only_enabled_ = false;
  }
  ir_depth_only_enabled_ = true;
  EtronDI_EnableInterleave(etron_di_, &dev_sel_info_, true);
  framerate_ *= 2;
}

void Device::SetInfraredIntensity(std::uint16_t value) {
  if (value != 0) {
    EtronDI_SetIRMode(etron_di_, &dev_sel_info_, 0x03);
    EtronDI_SetCurrentIRValue(etron_di_, &dev_sel_info_, value);
  } else {
    EtronDI_SetCurrentIRValue(etron_di_, &dev_sel_info_, value);
    EtronDI_SetIRMode(etron_di_, &dev_sel_info_, 0x00);
  }
}

bool Device::Open(const OpenParams& params) {
  if (params.stream_mode == StreamMode::STREAM_2560x720 &&
      params.framerate > 30) {
    LOGW("The framerate is too large, please use a smaller value (<=30).");
    return false;
  } else if (params.framerate > 60) {
    LOGW("The framerate is too large, please use a smaller value (<=60).");
    return false;
  }

  dev_sel_info_.index = params.dev_index;

  if (params.depth_mode == DepthMode::DEPTH_RAW) {
    // depth_raw ✓ depth_colorful x
    switch (params.color_mode) {
      case ColorMode::COLOR_RECTIFIED:
        depth_data_type_ = 2;  // ETronDI_DEPTH_DATA_14_BITS
        break;
      case ColorMode::COLOR_RAW:
      default:
        depth_data_type_ = 7;  // ETronDI_DEPTH_DATA_14_BITS_RAW
        break;
    }
  } else {
    // depth_raw x depth_colorful ✓
    switch (params.color_mode) {
      case ColorMode::COLOR_RECTIFIED:
        depth_data_type_ = 4;  // ETronDI_DEPTH_DATA_11_BITS
        break;
      case ColorMode::COLOR_RAW:
      default:
        depth_data_type_ = 9;  // ETronDI_DEPTH_DATA_11_BITS_RAW
        break;
    }
  }

  SetAutoExposureEnabled(params.state_ae);
  SetAutoWhiteBalanceEnabled(params.state_awb);

  if (params.framerate > 0) framerate_ = params.framerate;

#ifdef MYNTEYE_OS_LINUX
  std::string dtc_name = "Unknown";
  switch (params.depth_mode) {
    case DepthMode::DEPTH_GRAY:
      dtc_ = DEPTH_IMG_GRAY_TRANSFER;
      dtc_name = "Gray";
      break;
    case DepthMode::DEPTH_COLORFUL:
      dtc_ = DEPTH_IMG_COLORFUL_TRANSFER;
      dtc_name = "Colorful";
      break;
    case DepthMode::DEPTH_RAW:
    default:
      dtc_ = DEPTH_IMG_NON_TRANSFER;
      dtc_name = "Raw";
      break;
  }
#endif
  depth_mode_ = params.depth_mode;

  if (params.dev_index != stream_info_dev_index_) {
    std::vector<StreamInfo> color_infos;
    std::vector<StreamInfo> depth_infos;
    GetStreamInfos(params.dev_index, &color_infos, &depth_infos);
  }

  GetStreamIndex(params, &color_res_index_, &depth_res_index_);

  CompatibleUSB2(params);
  CompatibleMJPG();

  LOGI("-- Framerate: %d", framerate_);

  EtronDI_SetDepthDataType(etron_di_, &dev_sel_info_, depth_data_type_);
  DBG_LOGI("SetDepthDataType: %d", depth_data_type_);
  LOGI(" SetDepthDataType: %d", depth_data_type_);

  LOGI("-- Color Stream: %dx%d %s",
      stream_color_info_ptr_[color_res_index_].nWidth,
      stream_color_info_ptr_[color_res_index_].nHeight,
      stream_color_info_ptr_[color_res_index_].bFormatMJPG ? "MJPG" : "YUYV");
  LOGI("-- Depth Stream: %dx%d %s",
      stream_depth_info_ptr_[depth_res_index_].nWidth,
      stream_depth_info_ptr_[depth_res_index_].nHeight,
      stream_depth_info_ptr_[depth_res_index_].bFormatMJPG ? "MJPG" : "YUYV");

  SetInfraredDepthOnly(params);

  if (params.ir_intensity >= 0) {
    SetInfraredIntensity(params.ir_intensity);
    LOGI("\n-- IR intensity: %d", params.ir_intensity);
  }

  ReleaseBuf();

  int ret = OpenDevice(params.dev_mode);

  if (ETronDI_OK == ret) {
    open_params_ = params;
    if (depth_device_opened_) {
      // depth device must be opened.
      SyncCameraCalibrations();
    }
    return true;
  } else {
    dev_sel_info_.index = -1;  // reset flag
    return false;
  }
}

bool Device::IsOpened() const {
  return dev_sel_info_.index != -1;
}

void Device::CheckOpened(const std::string& event) const {
  if (!IsOpened()) {
    std::stringstream s;
    s << "Error: Camera must be opened";
    if (!event.empty()) {
      s << ", before " << event;
    }
    throw_error(s.str());
  }
}

bool Device::ExpectOpened(const std::string& event) const {
  if (IsOpened()) {
    return true;
  } else {
    LOGW("Warning: Camera should be opened, before %s", event);
    return false;
  }
}

OpenParams Device::GetOpenParams() const {
  return open_params_;
}

bool Device::IsRightColorSupported() const {
  CheckOpened(__func__);
  return IsRightColorSupported(open_params_.stream_mode);
}

bool Device::IsRightColorSupported(const StreamMode& stream_mode) const {
  return stream_mode == StreamMode::STREAM_1280x480
      || stream_mode == StreamMode::STREAM_2560x720;
}

std::shared_ptr<CameraCalibration> Device::GetCameraCalibration(
    const StreamMode& stream_mode) {
  switch (stream_mode) {
    case StreamMode::STREAM_640x480:
    case StreamMode::STREAM_1280x480:  // 480p, vga
      return GetCameraCalibration(1);
    case StreamMode::STREAM_1280x720:
    case StreamMode::STREAM_2560x720:  // 720p, hd
      return GetCameraCalibration(0);
    default:
      throw new std::runtime_error("StreamMode is unknown");
  }
}

bool Device::GetCameraCalibrationFile(const StreamMode& stream_mode,
                                      const std::string& filename) {
  switch (stream_mode) {
    case StreamMode::STREAM_640x480:
    case StreamMode::STREAM_1280x480:  // 480p, vga
      return GetCameraCalibrationFile(1, filename);
    case StreamMode::STREAM_1280x720:
    case StreamMode::STREAM_2560x720:  // 720p, hd
      return GetCameraCalibrationFile(0, filename);
    default:
      throw new std::runtime_error("StreamMode is unknown");
  }
}

bool Device::SetCameraCalibrationBinFile(const std::string& filename) {
  std::ifstream t;
  int length;
  t.open(filename.c_str());
  t.seekg(0, std::ios::end);
  length = t.tellg();
  t.seekg(0, std::ios::beg);
  char* buffer = new char[length];
  t.read(buffer, length);
  t.close();

  int nActualLength = 0;

  bool ok = (ETronDI_OK == EtronDI_SetLogData(etron_di_, &dev_sel_info_,
      (unsigned char*)buffer, length, &nActualLength, 0));
  if (!ok) printf("error when setLogData\n");
  delete[] buffer;

  SyncCameraCalibrations();
  return ok;
}

void Device::Close() {
  if (dev_sel_info_.index != -1) {
    EtronDI_CloseDevice(etron_di_, &dev_sel_info_);
    dev_sel_info_.index = -1;
  }
  ReleaseBuf();
  EtronDI_Release(&etron_di_);
}

void Device::GetStreamIndex(const OpenParams& params,
    int* color_res_index,
    int* depth_res_index) {
  return GetStreamIndex(
      params.dev_index, params.stream_mode,
      params.color_stream_format, params.depth_stream_format,
      color_res_index, depth_res_index);
}

void Device::GetStreamIndex(const std::int32_t& dev_index,
    const StreamMode& stream_mode,
    const StreamFormat& color_stream_format,
    const StreamFormat& depth_stream_format,
    int* color_res_index,
    int* depth_res_index) {
  if (!color_res_index) {
    LOGE("GetStreamIndex: color_res_index is null.");
    return;
  }
  if (!depth_res_index) {
    LOGE("GetStreamIndex: depth_res_index is null.");
    return;
  }

  *color_res_index = -1;
  *depth_res_index = -1;

  int width = 0, height = 0;
  get_stream_size(stream_mode, &width, &height);

  memset(stream_color_info_ptr_, 0, sizeof(ETRONDI_STREAM_INFO)*64);
  memset(stream_depth_info_ptr_, 0, sizeof(ETRONDI_STREAM_INFO)*64);

  DEVSELINFO dev_sel_info{dev_index};
  EtronDI_GetDeviceResolutionList(etron_di_, &dev_sel_info, 64,
      stream_color_info_ptr_, 64, stream_depth_info_ptr_);

  PETRONDI_STREAM_INFO stream_temp_info_ptr = stream_color_info_ptr_;
  int i = 0;
  while (i < 64) {
    if (stream_temp_info_ptr->nWidth == width &&
        stream_temp_info_ptr->nHeight == height &&
        color_stream_format == (stream_temp_info_ptr->bFormatMJPG ?
            StreamFormat::STREAM_MJPG : StreamFormat::STREAM_YUYV)) {
      *color_res_index = i;
      break;
    }
    stream_temp_info_ptr++;
    i++;
  }

  if (*color_res_index == -1) {
    LOGE("Error: Color Mode width[%d] height[%d] format[%s] not support. "
        "Please check the resolution list.", width, height,
        get_stream_format_string(color_stream_format));
    *color_res_index = 0;
  }

  stream_temp_info_ptr = stream_depth_info_ptr_;
  i = 0;
  while (i < 64) {
    if (stream_temp_info_ptr->nHeight == height &&
        depth_stream_format == (stream_temp_info_ptr->bFormatMJPG ?
            StreamFormat::STREAM_MJPG : StreamFormat::STREAM_YUYV)) {
      *depth_res_index = i;
      break;
    }
    stream_temp_info_ptr++;
    i++;
  }

  if (*depth_res_index == -1) {
    LOGE("Error: Depth Mode width[%d] height[%d] format[%s] not support. "
        "Please check the resolution list.", width, height,
        get_stream_format_string(depth_stream_format));
    *depth_res_index = 0;
  }
}

bool Device::GetSensorRegister(int id, std::uint16_t address,
    std::uint16_t* value, int flag) {
  if (!ExpectOpened(__func__)) return false;
#ifdef MYNTEYE_OS_WIN
  return ETronDI_OK == EtronDI_GetSensorRegister(etron_di_, &dev_sel_info_, id,
      address, value, flag, 2);
#else
  return ETronDI_OK == EtronDI_GetSensorRegister(etron_di_, &dev_sel_info_, id,
      address, value, flag, SENSOR_BOTH);
#endif
}

bool Device::GetHWRegister(std::uint16_t address, std::uint16_t* value,
    int flag) {
  if (!ExpectOpened(__func__)) return false;
  return ETronDI_OK == EtronDI_GetHWRegister(etron_di_, &dev_sel_info_,
      address, value, flag);
}

bool Device::GetFWRegister(std::uint16_t address, std::uint16_t* value,
    int flag) {
  if (!ExpectOpened(__func__)) return false;
  return ETronDI_OK == EtronDI_GetFWRegister(etron_di_, &dev_sel_info_, address,
      value, flag);
}

bool Device::SetSensorRegister(int id, std::uint16_t address,
    std::uint16_t value, int flag) {
  if (!ExpectOpened(__func__)) return false;
#ifdef MYNTEYE_OS_WIN
  return ETronDI_OK == EtronDI_SetSensorRegister(etron_di_, &dev_sel_info_, id,
      address, value, flag, 2);
#else
  return ETronDI_OK == EtronDI_SetSensorRegister(etron_di_, &dev_sel_info_, id,
      address, value, flag, SENSOR_BOTH);
#endif
}

bool Device::SetHWRegister(std::uint16_t address, std::uint16_t value,
    int flag) {
  if (!ExpectOpened(__func__)) return false;
  return ETronDI_OK == EtronDI_SetHWRegister(etron_di_, &dev_sel_info_, address,
      value, flag);
}

bool Device::SetFWRegister(std::uint16_t address, std::uint16_t value,
    int flag) {
  if (!ExpectOpened(__func__)) return false;
  return ETronDI_OK == EtronDI_SetFWRegister(etron_di_, &dev_sel_info_, address,
      value, flag);
}

std::shared_ptr<CameraCalibration> Device::GetCameraCalibration(int index) {
  return camera_calibrations_[index];
}

bool Device::GetCameraCalibrationFile(int index, const std::string& filename) {
  if (!ExpectOpened(__func__)) return false;
  int nRet;

  // for parse log test
  eSPCtrl_RectLogData eSPRectLogData;
  // EtronDI_GetRectifyLogData in puma
  // EtronDI_GetRectifyMatLogData
  nRet = EtronDI_GetRectifyMatLogData(etron_di_,
    &dev_sel_info_, &eSPRectLogData, index);
  // printf("nRet = %d", nRet);
  if (nRet != ETronDI_OK) {
    return false;
  }

  FILE *pfile;
  // char buf[128];
  // sprintf(buf, "RectfyLog_PUMA_%d.txt", index);
  pfile = fopen(/*buf*/filename.c_str(), "wt");
  if (pfile != NULL) {
    int i;
    fprintf(pfile, "InImgWidth = %d\n",        eSPRectLogData.InImgWidth);
    fprintf(pfile, "InImgHeight = %d\n",       eSPRectLogData.InImgHeight);
    fprintf(pfile, "OutImgWidth = %d\n",       eSPRectLogData.OutImgWidth);
    fprintf(pfile, "OutImgHeight = %d\n",      eSPRectLogData.OutImgHeight);
    //
    fprintf(pfile, "RECT_ScaleWidth = %d\n",   eSPRectLogData.RECT_ScaleWidth);
    fprintf(pfile, "RECT_ScaleHeight = %d\n",  eSPRectLogData.RECT_ScaleHeight);
    //
    fprintf(pfile, "CamMat1 = ");
    for (i=0; i < 9; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.CamMat1[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "CamDist1 = ");
    for (i=0; i < 8; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.CamDist1[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "CamMat2 = ");
    for (i=0; i < 9; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.CamMat2[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "CamDist2 = ");
    for (i=0; i < 8; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.CamDist2[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "RotaMat = ");
    for (i=0; i < 9; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.RotaMat[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "TranMat = ");
    for (i=0; i < 3; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.TranMat[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "LRotaMat = ");
    for (i=0; i < 9; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.LRotaMat[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "RRotaMat = ");
    for (i=0; i < 9; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.RRotaMat[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "NewCamMat1 = ");
    for (i=0; i < 12; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.NewCamMat1[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "NewCamMat2 = ");
    for (i=0; i < 12; i++) {
        fprintf(pfile, "%.8f, ",  eSPRectLogData.NewCamMat2[i]);
    }
    fprintf(pfile, "\n");
    //
    fprintf(pfile, "RECT_Crop_Row_BG = %d\n",
      eSPRectLogData.RECT_Crop_Row_BG);
    fprintf(pfile, "RECT_Crop_Row_ED = %d\n",
      eSPRectLogData.RECT_Crop_Row_ED);
    fprintf(pfile, "RECT_Crop_Col_BG_L = %d\n",
      eSPRectLogData.RECT_Crop_Col_BG_L);
    fprintf(pfile, "RECT_Crop_Col_ED_L = %d\n",
      eSPRectLogData.RECT_Crop_Col_ED_L);
    fprintf(pfile, "RECT_Scale_Col_M = %d\n",
      eSPRectLogData.RECT_Scale_Col_M);
    fprintf(pfile, "RECT_Scale_Col_N = %d\n",
      eSPRectLogData.RECT_Scale_Col_N);
    fprintf(pfile, "RECT_Scale_Row_M = %d\n",
      eSPRectLogData.RECT_Scale_Row_M);
    fprintf(pfile, "RECT_Scale_Row_N = %d\n",
      eSPRectLogData.RECT_Scale_Row_N);
    //
    fprintf(pfile, "RECT_AvgErr = %.8f\n", eSPRectLogData.RECT_AvgErr);
    //
    fprintf(pfile, "nLineBuffers = %d\n",  eSPRectLogData.nLineBuffers);
    //
    printf("file ok\n");
    fprintf(pfile, "ReProjectMat = ");
    for (i = 0; i < 16; i++) {
      fprintf(pfile, "%.8f, ", eSPRectLogData.ReProjectMat[i]);
    }
    fprintf(pfile, "\n");
  }
  fclose(pfile);
  return true;
}

void Device::SyncCameraCalibrations() {
  if (!ExpectOpened(__func__)) return;
  camera_calibrations_.clear();
  for (int index = 0; index < 2; index++) {
    eSPCtrl_RectLogData eSPRectLogData;
    int ret = EtronDI_GetRectifyMatLogData(etron_di_, &dev_sel_info_,
        &eSPRectLogData, index);
    if (ret != ETronDI_OK) {
      return;
    }
    int i;
    auto camera_calib = std::make_shared<CameraCalibration>();
    camera_calib->InImgWidth = eSPRectLogData.InImgWidth;
    camera_calib->InImgHeight = eSPRectLogData.InImgHeight;
    camera_calib->OutImgWidth = eSPRectLogData.OutImgWidth;
    camera_calib->OutImgHeight = eSPRectLogData.OutImgHeight;
    camera_calib->RECT_ScaleWidth = eSPRectLogData.RECT_ScaleWidth;
    camera_calib->RECT_ScaleHeight = eSPRectLogData.RECT_ScaleHeight;
    for (i=0; i < 9; i++) {
      camera_calib->CamMat1[i] = eSPRectLogData.CamMat1[i];
    }
    for (i=0; i < 8; i++) {
      camera_calib->CamDist1[i] = eSPRectLogData.CamDist1[i];
    }
    for (i=0; i < 9; i++) {
      camera_calib->CamMat2[i] = eSPRectLogData.CamMat2[i];
    }
    for (i=0; i < 8; i++) {
      camera_calib->CamDist2[i] = eSPRectLogData.CamDist2[i];
    }
    for (i=0; i < 9; i++) {
      camera_calib->RotaMat[i] = eSPRectLogData.RotaMat[i];
    }
    for (i=0; i < 3; i++) {
      camera_calib->TranMat[i] = eSPRectLogData.TranMat[i];
    }
    for (i=0; i < 9; i++) {
      camera_calib->LRotaMat[i] = eSPRectLogData.LRotaMat[i];
    }
    for (i=0; i < 9; i++) {
      camera_calib->RRotaMat[i] = eSPRectLogData.RRotaMat[i];
    }
    for (i=0; i < 12; i++) {
      camera_calib->NewCamMat1[i] = eSPRectLogData.NewCamMat1[i];
    }
    for (i=0; i < 12; i++) {
      camera_calib->NewCamMat2[i] = eSPRectLogData.NewCamMat2[i];
    }
    camera_calib->RECT_Crop_Row_BG = eSPRectLogData.RECT_Crop_Row_BG;
    camera_calib->RECT_Crop_Row_ED = eSPRectLogData.RECT_Crop_Row_ED;
    camera_calib->RECT_Crop_Col_BG_L = eSPRectLogData.RECT_Crop_Col_BG_L;
    camera_calib->RECT_Crop_Col_ED_L = eSPRectLogData.RECT_Crop_Col_ED_L;
    camera_calib->RECT_Scale_Col_M = eSPRectLogData.RECT_Scale_Col_M;
    camera_calib->RECT_Scale_Col_N = eSPRectLogData.RECT_Scale_Col_N;
    camera_calib->RECT_Scale_Row_M = eSPRectLogData.RECT_Scale_Row_M;
    camera_calib->RECT_Scale_Row_N = eSPRectLogData.RECT_Scale_Row_N;
    camera_calib->RECT_AvgErr = eSPRectLogData.RECT_AvgErr;
    camera_calib->nLineBuffers = eSPRectLogData.nLineBuffers;
    for (i = 0; i < 16; i++) {
      camera_calib->ReProjectMat[i] = eSPRectLogData.ReProjectMat[i];
    }
    camera_calibrations_.push_back(camera_calib);
  }
}

void Device::ReleaseBuf() {
  color_image_buf_ = nullptr;
  depth_image_buf_ = nullptr;
  if (!depth_buf_) {
    delete depth_buf_;
    depth_buf_ = nullptr;
  }
}

void Device::CompatibleUSB2(const OpenParams& params) {
  if (!IsUSB2()) {
    return;
  }

  if ((params.stream_mode == StreamMode::STREAM_1280x720 &&
        framerate_ <= 5) ||
      (params.stream_mode == StreamMode::STREAM_640x480 &&
       framerate_ <= 15) ||
      (params.stream_mode == StreamMode::STREAM_1280x720 &&
       framerate_ <= 10 &&
       params.dev_mode != DeviceMode::DEVICE_ALL)) {
    LOGI("\nWARNING:: You are using the USB 2.0 interface. "
        "For bandwidth reasons, "
        "it is recommended to use the USB 3.0 interface.\n");
    // 8bit 1, 6 match 14bit 2, 7
    depth_data_type_ = (depth_data_type_ == 7) ? 6 : 1;
  } else {
    throw_error("\nNote:: You are using the USB 2.0 interface."
        " Current resolution or frame rate is not supported"
        " And you can refer to Resolution Support List"
        " in the documentation.\n");
  }
}

void Device::CompatibleMJPG() {
  if (!stream_color_info_ptr_[color_res_index_].bFormatMJPG) {
    return;
  }
  // 8bit 1, 6 match 14bit 2, 7
  // Now, only support 1
  // depth_data_type_ = (depth_data_type_ == 7) ? 6 : 1;
  depth_data_type_ = 1;
}

bool Device::IsUSB2() {
  if (stream_depth_info_ptr_ == nullptr) {
    return false;
  }

  return stream_depth_info_ptr_[1].nWidth == 320;
}

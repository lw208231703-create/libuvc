/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (C) 2010-2012 Ken Tossell
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
*   * Neither the name of the author nor other contributors may be
*     used to endorse or promote products derived from this software
*     without specific prior written permission.
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
/**
 * @defgroup streaming Streaming control functions
 * @brief Tools for creating, managing and consuming video streams
 */

#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include "errno.h"

#ifdef _MSC_VER

#define DELTA_EPOCH_IN_MICROSECS  116444736000000000Ui64

// gettimeofday - get time of day for Windows;
// A gettimeofday implementation for Microsoft Windows;
// Public domain code, author "ponnada";
int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag = 0;
    if (NULL != tv)
    {
        GetSystemTimeAsFileTime(&ft);
        tmpres |= ft.dwHighDateTime;
        tmpres <<= 32;
        tmpres |= ft.dwLowDateTime;
        tmpres /= 10;
        tmpres -= DELTA_EPOCH_IN_MICROSECS;
        tv->tv_sec = (long)(tmpres / 1000000UL);
        tv->tv_usec = (long)(tmpres % 1000000UL);
    }
    return 0;
}
#endif // _MSC_VER
uvc_frame_desc_t *uvc_find_frame_desc_stream(uvc_stream_handle_t *strmh,
    uint16_t format_id, uint16_t frame_id);
uvc_frame_desc_t *uvc_find_frame_desc(uvc_device_handle_t *devh,
    uint16_t format_id, uint16_t frame_id);
void *_uvc_user_caller(void *arg);
void _uvc_populate_frame(uvc_stream_handle_t *strmh);

static uvc_streaming_interface_t *_uvc_get_stream_if(uvc_device_handle_t *devh, int interface_idx);
static uvc_stream_handle_t *_uvc_get_stream_by_interface(uvc_device_handle_t *devh, int interface_idx);

static void _uvc_clear_stream_endpoint(uvc_stream_handle_t *strmh, const char *reason) {
  uint8_t endpoint;
  int ret;

  if (!strmh || !strmh->stream_if)
    return;

  endpoint = strmh->stream_if->bEndpointAddress;
  if (!endpoint)
    return;

  ret = libusb_clear_halt(strmh->devh->usb_devh, endpoint);
  fprintf(stderr, "[libuvc] clear_halt(%s) endpoint=0x%02x ret=%d\n",
          reason ? reason : "stream", endpoint, ret);
}

static void _uvc_set_stream_interface_alt0(uvc_stream_handle_t *strmh, const char *reason) {
  int ret;

  if (!strmh || !strmh->stream_if)
    return;

  ret = libusb_set_interface_alt_setting(strmh->devh->usb_devh,
                                         strmh->stream_if->bInterfaceNumber,
                                         0);
  fprintf(stderr, "[libuvc] set_interface_alt0(%s) interface=%u ret=%d\n",
          reason ? reason : "stream", strmh->stream_if->bInterfaceNumber, ret);
}

struct format_table_entry {
  enum uvc_frame_format format;
  uint8_t abstract_fmt;
  uint8_t guid[16];
  int children_count;
  enum uvc_frame_format *children;
};

struct format_table_entry *_get_format_entry(enum uvc_frame_format format) {
  #define ABS_FMT(_fmt, _num, ...) \
    case _fmt: { \
    static enum uvc_frame_format _fmt##_children[] = __VA_ARGS__; \
    static struct format_table_entry _fmt##_entry = { \
      _fmt, 0, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, _num, _fmt##_children }; \
    return &_fmt##_entry; }

  #define FMT(_fmt, ...) \
    case _fmt: { \
    static struct format_table_entry _fmt##_entry = { \
      _fmt, 0, __VA_ARGS__, 0, NULL }; \
    return &_fmt##_entry; }

  switch(format) {
    /* Define new formats here */
    ABS_FMT(UVC_FRAME_FORMAT_ANY, 2,
      {UVC_FRAME_FORMAT_UNCOMPRESSED, UVC_FRAME_FORMAT_COMPRESSED})

    ABS_FMT(UVC_FRAME_FORMAT_UNCOMPRESSED, 8,
      {UVC_FRAME_FORMAT_YUYV, UVC_FRAME_FORMAT_UYVY, UVC_FRAME_FORMAT_GRAY8,
       UVC_FRAME_FORMAT_GRAY16, UVC_FRAME_FORMAT_NV12, UVC_FRAME_FORMAT_P010,
       UVC_FRAME_FORMAT_BGR, UVC_FRAME_FORMAT_RGB})
    FMT(UVC_FRAME_FORMAT_YUYV,
      {'Y',  'U',  'Y',  '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_UYVY,
      {'U',  'Y',  'V',  'Y', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_GRAY8,
      {'Y',  '8',  '0',  '0', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_GRAY16,
      {'Y',  '1',  '6',  ' ', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_NV12,
      {'N',  'V',  '1',  '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_P010,
      {'P',  '0',  '1',  '0', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_BGR,
      {0x7d, 0xeb, 0x36, 0xe4, 0x4f, 0x52, 0xce, 0x11, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70})
    FMT(UVC_FRAME_FORMAT_RGB,
        {0x7e, 0xeb, 0x36, 0xe4, 0x4f, 0x52, 0xce, 0x11, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70})
    FMT(UVC_FRAME_FORMAT_BY8,
      {'B',  'Y',  '8',  ' ', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_BA81,
      {'B',  'A',  '8',  '1', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_SGRBG8,
      {'G',  'R',  'B',  'G', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_SGBRG8,
      {'G',  'B',  'R',  'G', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_SRGGB8,
      {'R',  'G',  'G',  'B', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    FMT(UVC_FRAME_FORMAT_SBGGR8,
      {'B',  'G',  'G',  'R', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
    ABS_FMT(UVC_FRAME_FORMAT_COMPRESSED, 2,
      {UVC_FRAME_FORMAT_MJPEG, UVC_FRAME_FORMAT_H264})
    FMT(UVC_FRAME_FORMAT_MJPEG,
      {'M',  'J',  'P',  'G'})
    FMT(UVC_FRAME_FORMAT_H264,
      {'H',  '2',  '6',  '4', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})

    default:
      return NULL;
  }

  #undef ABS_FMT
  #undef FMT
}

static uint8_t _uvc_frame_format_matches_guid(enum uvc_frame_format fmt, uint8_t guid[16]) {
  struct format_table_entry *format;
  int child_idx;

  format = _get_format_entry(fmt);
  if (!format)
    return 0;

  if (!format->abstract_fmt && !memcmp(guid, format->guid, 16))
    return 1;

  for (child_idx = 0; child_idx < format->children_count; child_idx++) {
    if (_uvc_frame_format_matches_guid(format->children[child_idx], guid))
      return 1;
  }

  return 0;
}

static enum uvc_frame_format uvc_frame_format_for_guid(uint8_t guid[16]) {
  struct format_table_entry *format;
  enum uvc_frame_format fmt;

  for (fmt = 0; fmt < UVC_FRAME_FORMAT_COUNT; ++fmt) {
    format = _get_format_entry(fmt);
    if (!format || format->abstract_fmt)
      continue;
    if (!memcmp(format->guid, guid, 16))
      return format->format;
  }

  return UVC_FRAME_FORMAT_UNKNOWN;
}

/** @internal
 * Run a streaming control query
 * @param[in] devh UVC device
 * @param[in,out] ctrl Control block
 * @param[in] probe Whether this is a probe query or a commit query
 * @param[in] req Query type
 */
uvc_error_t uvc_query_stream_ctrl(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl,
    uint8_t probe,
    enum uvc_req_code req) {
  uint8_t buf[34];
  size_t len;
  uvc_error_t err;

  memset(buf, 0, sizeof(buf));

  if (devh->info->ctrl_if.bcdUVC >= 0x0110)
    len = 34;
  else
    len = 26;

  /* prepare for a SET transfer */
  if (req == UVC_SET_CUR) {
    SHORT_TO_SW(ctrl->bmHint, buf);
    buf[2] = ctrl->bFormatIndex;
    buf[3] = ctrl->bFrameIndex;
    INT_TO_DW(ctrl->dwFrameInterval, buf + 4);
    SHORT_TO_SW(ctrl->wKeyFrameRate, buf + 8);
    SHORT_TO_SW(ctrl->wPFrameRate, buf + 10);
    SHORT_TO_SW(ctrl->wCompQuality, buf + 12);
    SHORT_TO_SW(ctrl->wCompWindowSize, buf + 14);
    SHORT_TO_SW(ctrl->wDelay, buf + 16);
    INT_TO_DW(ctrl->dwMaxVideoFrameSize, buf + 18);
    INT_TO_DW(ctrl->dwMaxPayloadTransferSize, buf + 22);

    if (len == 34) {
      INT_TO_DW ( ctrl->dwClockFrequency, buf + 26 );
      buf[30] = ctrl->bmFramingInfo;
      buf[31] = ctrl->bPreferredVersion;
      buf[32] = ctrl->bMinVersion;
      buf[33] = ctrl->bMaxVersion;
      /** @todo support UVC 1.1 */
    }
  }

  /* do the transfer */
  err = libusb_control_transfer(
      devh->usb_devh,
      req == UVC_SET_CUR ? 0x21 : 0xA1,
      req,
      probe ? (UVC_VS_PROBE_CONTROL << 8) : (UVC_VS_COMMIT_CONTROL << 8),
      ctrl->bInterfaceNumber,
      buf, len, 0
  );

  if (err <= 0) {
    return err;
  }

  /* now decode following a GET transfer */
  if (req != UVC_SET_CUR) {
    ctrl->bmHint = SW_TO_SHORT(buf);
    ctrl->bFormatIndex = buf[2];
    ctrl->bFrameIndex = buf[3];
    ctrl->dwFrameInterval = DW_TO_INT(buf + 4);
    ctrl->wKeyFrameRate = SW_TO_SHORT(buf + 8);
    ctrl->wPFrameRate = SW_TO_SHORT(buf + 10);
    ctrl->wCompQuality = SW_TO_SHORT(buf + 12);
    ctrl->wCompWindowSize = SW_TO_SHORT(buf + 14);
    ctrl->wDelay = SW_TO_SHORT(buf + 16);
    ctrl->dwMaxVideoFrameSize = DW_TO_INT(buf + 18);
    ctrl->dwMaxPayloadTransferSize = DW_TO_INT(buf + 22);

    if (len == 34) {
      ctrl->dwClockFrequency = DW_TO_INT ( buf + 26 );
      ctrl->bmFramingInfo = buf[30];
      ctrl->bPreferredVersion = buf[31];
      ctrl->bMinVersion = buf[32];
      ctrl->bMaxVersion = buf[33];
      /** @todo support UVC 1.1 */
    }
    else
      ctrl->dwClockFrequency = devh->info->ctrl_if.dwClockFrequency;

    /* fix up block for cameras that fail to set dwMax* */
    if (ctrl->dwMaxVideoFrameSize == 0) {
      uvc_frame_desc_t *frame = uvc_find_frame_desc(devh, ctrl->bFormatIndex, ctrl->bFrameIndex);

      if (frame) {
        ctrl->dwMaxVideoFrameSize = frame->dwMaxVideoFrameBufferSize;
      }
    }
  }

  return UVC_SUCCESS;
}

/** @internal
 * Run a streaming control query
 * @param[in] devh UVC device
 * @param[in,out] ctrl Control block
 * @param[in] probe Whether this is a probe query or a commit query
 * @param[in] req Query type
 */
uvc_error_t uvc_query_still_ctrl(
  uvc_device_handle_t *devh,
  uvc_still_ctrl_t *still_ctrl,
  uint8_t probe,
  enum uvc_req_code req) {

  uint8_t buf[11];
  const size_t len = 11;
  uvc_error_t err;

  memset(buf, 0, sizeof(buf));

  if (req == UVC_SET_CUR) {
    /* prepare for a SET transfer */
    buf[0] = still_ctrl->bFormatIndex;
    buf[1] = still_ctrl->bFrameIndex;
    buf[2] = still_ctrl->bCompressionIndex;
    INT_TO_DW(still_ctrl->dwMaxVideoFrameSize, buf + 3);
    INT_TO_DW(still_ctrl->dwMaxPayloadTransferSize, buf + 7);
  }

  /* do the transfer */
  err = libusb_control_transfer(
      devh->usb_devh,
      req == UVC_SET_CUR ? 0x21 : 0xA1,
      req,
      probe ? (UVC_VS_STILL_PROBE_CONTROL << 8) : (UVC_VS_STILL_COMMIT_CONTROL << 8),
      still_ctrl->bInterfaceNumber,
      buf, len, 0
  );

  if (err <= 0) {
    return err;
  }

  /* now decode following a GET transfer */
  if (req != UVC_SET_CUR) {
    still_ctrl->bFormatIndex = buf[0];
    still_ctrl->bFrameIndex = buf[1];
    still_ctrl->bCompressionIndex = buf[2];
    still_ctrl->dwMaxVideoFrameSize = DW_TO_INT(buf + 3);
    still_ctrl->dwMaxPayloadTransferSize = DW_TO_INT(buf + 7);
  }

  return UVC_SUCCESS;
}

/** Initiate a method 2 (in stream) still capture
 * @ingroup streaming
 *
 * @param[in] devh Device handle
 * @param[in] still_ctrl Still capture control block
 */
uvc_error_t uvc_trigger_still(
    uvc_device_handle_t *devh,
    uvc_still_ctrl_t *still_ctrl) {
  uvc_stream_handle_t* stream;
  uvc_streaming_interface_t* stream_if;
  uint8_t buf;
  uvc_error_t err;

  /* Stream must be running for method 2 to work */
  stream = _uvc_get_stream_by_interface(devh, still_ctrl->bInterfaceNumber);
  if (!stream || !stream->running)
    return UVC_ERROR_NOT_SUPPORTED;

  /* Only method 2 is supported */
  stream_if = _uvc_get_stream_if(devh, still_ctrl->bInterfaceNumber);
  if(!stream_if || stream_if->bStillCaptureMethod != 2)
      return UVC_ERROR_NOT_SUPPORTED;

  /* prepare for a SET transfer */
  buf = 1;

  /* do the transfer */
  err = libusb_control_transfer(
      devh->usb_devh,
      0x21, //type set
      UVC_SET_CUR,
      (UVC_VS_STILL_IMAGE_TRIGGER_CONTROL << 8),
      still_ctrl->bInterfaceNumber,
      &buf, 1, 0);

  if (err <= 0) {
    return err;
  }

  return UVC_SUCCESS;
}

/** @brief Reconfigure stream with a new stream format.
 * @ingroup streaming
 *
 * This may be executed whether or not the stream is running.
 *
 * @param[in] strmh Stream handle
 * @param[in] ctrl Control block, processed using {uvc_probe_stream_ctrl} or
 *             {uvc_get_stream_ctrl_format_size}
 */
uvc_error_t uvc_stream_ctrl(uvc_stream_handle_t *strmh, uvc_stream_ctrl_t *ctrl) {
  uvc_error_t ret;

  if (strmh->stream_if->bInterfaceNumber != ctrl->bInterfaceNumber)
    return UVC_ERROR_INVALID_PARAM;

  /* @todo Allow the stream to be modified without restarting the stream */
  if (strmh->running)
    return UVC_ERROR_BUSY;

  ret = uvc_query_stream_ctrl(strmh->devh, ctrl, 0, UVC_SET_CUR);
  if (ret != UVC_SUCCESS)
    return ret;

  strmh->cur_ctrl = *ctrl;
  return UVC_SUCCESS;
}

/** @internal
 * @brief Find the descriptor for a specific frame configuration
 * @param stream_if Stream interface
 * @param format_id Index of format class descriptor
 * @param frame_id Index of frame descriptor
 */
static uvc_frame_desc_t *_uvc_find_frame_desc_stream_if(uvc_streaming_interface_t *stream_if,
    uint16_t format_id, uint16_t frame_id) {
 
  uvc_format_desc_t *format = NULL;
  uvc_frame_desc_t *frame = NULL;

  DL_FOREACH(stream_if->format_descs, format) {
    if (format->bFormatIndex == format_id) {
      DL_FOREACH(format->frame_descs, frame) {
        if (frame->bFrameIndex == frame_id)
          return frame;
      }
    }
  }

  return NULL;
}

uvc_frame_desc_t *uvc_find_frame_desc_stream(uvc_stream_handle_t *strmh,
    uint16_t format_id, uint16_t frame_id) {
  return _uvc_find_frame_desc_stream_if(strmh->stream_if, format_id, frame_id);
}

/** @internal
 * @brief Find the descriptor for a specific frame configuration
 * @param devh UVC device
 * @param format_id Index of format class descriptor
 * @param frame_id Index of frame descriptor
 */
uvc_frame_desc_t *uvc_find_frame_desc(uvc_device_handle_t *devh,
    uint16_t format_id, uint16_t frame_id) {
 
  uvc_streaming_interface_t *stream_if;
  uvc_frame_desc_t *frame;

  DL_FOREACH(devh->info->stream_ifs, stream_if) {
    frame = _uvc_find_frame_desc_stream_if(stream_if, format_id, frame_id);
    if (frame)
      return frame;
  }

  return NULL;
}

/** Get a negotiated streaming control block for some common parameters.
 * @ingroup streaming
 *
 * @param[in] devh Device handle
 * @param[in,out] ctrl Control block
 * @param[in] format_class Type of streaming format
 * @param[in] width Desired frame width
 * @param[in] height Desired frame height
 * @param[in] fps Frame rate, frames per second
 */
uvc_error_t uvc_get_stream_ctrl_format_size(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl,
    enum uvc_frame_format cf,
    int width, int height,
    int fps) {
  uvc_streaming_interface_t *stream_if;

  /* find a matching frame descriptor and interval */
  DL_FOREACH(devh->info->stream_ifs, stream_if) {
    uvc_format_desc_t *format;

    DL_FOREACH(stream_if->format_descs, format) {
      uvc_frame_desc_t *frame;

      if (!_uvc_frame_format_matches_guid(cf, format->guidFormat))
        continue;

      DL_FOREACH(format->frame_descs, frame) {
        if (frame->wWidth != width || frame->wHeight != height)
          continue;

        uint32_t *interval;

        ctrl->bInterfaceNumber = stream_if->bInterfaceNumber;
        UVC_DEBUG("claiming streaming interface %d", stream_if->bInterfaceNumber );
        uvc_claim_if(devh, ctrl->bInterfaceNumber);
        /* get the max values */
        uvc_query_stream_ctrl( devh, ctrl, 1, UVC_GET_MAX);

        if (frame->intervals) {
          for (interval = frame->intervals; *interval; ++interval) {
            // allow a fps rate of zero to mean "accept first rate available"
            if (10000000 / *interval == (unsigned int) fps || fps == 0) {

              ctrl->bmHint = (1 << 0); /* don't negotiate interval */
              ctrl->bFormatIndex = format->bFormatIndex;
              ctrl->bFrameIndex = frame->bFrameIndex;
              ctrl->dwFrameInterval = *interval;

              goto found;
            }
          }
        } else {
          uint32_t interval_100ns = 10000000 / fps;
          uint32_t interval_offset = interval_100ns - frame->dwMinFrameInterval;

          if (interval_100ns >= frame->dwMinFrameInterval
              && interval_100ns <= frame->dwMaxFrameInterval
              && !(interval_offset
                   && (interval_offset % frame->dwFrameIntervalStep))) {

            ctrl->bmHint = (1 << 0);
            ctrl->bFormatIndex = format->bFormatIndex;
            ctrl->bFrameIndex = frame->bFrameIndex;
            ctrl->dwFrameInterval = interval_100ns;

            goto found;
          }
        }
      }
    }
  }

  return UVC_ERROR_INVALID_MODE;

found:
  return uvc_probe_stream_ctrl(devh, ctrl);
}

/** Get a negotiated still control block for some common parameters.
 * @ingroup streaming
 *
 * @param[in] devh Device handle
 * @param[in] ctrl Control block
 * @param[in, out] still_ctrl Still capture control block
 * @param[in] width Desired frame width
 * @param[in] height Desired frame height
 */
uvc_error_t uvc_get_still_ctrl_format_size(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl,
    uvc_still_ctrl_t *still_ctrl,
    int width, int height) {
  uvc_streaming_interface_t *stream_if;
  uvc_still_frame_desc_t *still;
  uvc_format_desc_t *format;
  uvc_still_frame_res_t *sizePattern;

  stream_if = _uvc_get_stream_if(devh, ctrl->bInterfaceNumber);

  /* Only method 2 is supported */
  if(!stream_if || stream_if->bStillCaptureMethod != 2)
    return UVC_ERROR_NOT_SUPPORTED;

  DL_FOREACH(stream_if->format_descs, format) {

    if (ctrl->bFormatIndex != format->bFormatIndex)
      continue;

    /* get the max values */
    uvc_query_still_ctrl(devh, still_ctrl, 1, UVC_GET_MAX);

    //look for still format
    DL_FOREACH(format->still_frame_desc, still) {
      DL_FOREACH(still->imageSizePatterns, sizePattern) {

        if (sizePattern->wWidth != width || sizePattern->wHeight != height)
          continue;

        still_ctrl->bInterfaceNumber = ctrl->bInterfaceNumber;
        still_ctrl->bFormatIndex = format->bFormatIndex;
        still_ctrl->bFrameIndex = sizePattern->bResolutionIndex;
        still_ctrl->bCompressionIndex = 0; //TODO support compression index
        goto found;
      }
    }
  }

  return UVC_ERROR_INVALID_MODE;

  found:
    return uvc_probe_still_ctrl(devh, still_ctrl);
}

static int _uvc_stream_params_negotiated(
  uvc_stream_ctrl_t *required,
  uvc_stream_ctrl_t *actual) {
    // 移除了 dwMaxPayloadTransferSize >= actual->dwMaxPayloadTransferSize 校验
    // 自研高带宽/非标相机的实际包大小可能超过默认极限值，放宽协商条件
    return required->bFormatIndex == actual->bFormatIndex &&
           required->bFrameIndex == actual->bFrameIndex;
}

/** @internal
 * Negotiate streaming parameters with the device
 *
 * @param[in] devh UVC device
 * @param[in,out] ctrl Control block
 */
uvc_error_t uvc_probe_stream_ctrl(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl) {
  uvc_stream_ctrl_t required_ctrl = *ctrl;

  uvc_query_stream_ctrl( devh, ctrl, 1, UVC_SET_CUR );
  uvc_query_stream_ctrl( devh, ctrl, 1, UVC_GET_CUR );

  if(!_uvc_stream_params_negotiated(&required_ctrl, ctrl)) {
    UVC_DEBUG("Unable to negotiate streaming format");
    return UVC_ERROR_INVALID_MODE;
  }

  return UVC_SUCCESS;
}

/** @internal
 * Negotiate still parameters with the device
 *
 * @param[in] devh UVC device
 * @param[in,out] still_ctrl Still capture control block
 */
uvc_error_t uvc_probe_still_ctrl(
    uvc_device_handle_t *devh,
    uvc_still_ctrl_t *still_ctrl) {

  int res = uvc_query_still_ctrl(
    devh, still_ctrl, 1, UVC_SET_CUR
  );

  if(res == UVC_SUCCESS) {
    res = uvc_query_still_ctrl(
      devh, still_ctrl, 1, UVC_GET_CUR
    );

    if(res == UVC_SUCCESS) {
      res = uvc_query_still_ctrl(
        devh, still_ctrl, 0, UVC_SET_CUR
      );
    }
  }

  return res;
}

/** @internal
 * @brief Swap the working buffer with the presented buffer and notify consumers
 */
void _uvc_swap_buffers(uvc_stream_handle_t *strmh) {
  uint8_t *tmp_buf;

  pthread_mutex_lock(&strmh->cb_mutex);

  (void)clock_gettime(CLOCK_MONOTONIC, &strmh->capture_time_finished);

  /* swap the buffers */
  tmp_buf = strmh->holdbuf;
  strmh->hold_bytes = strmh->got_bytes;
  strmh->holdbuf = strmh->outbuf;
  strmh->outbuf = tmp_buf;
  strmh->hold_last_scr = strmh->last_scr;
  strmh->hold_pts = strmh->pts;
  strmh->hold_seq = strmh->seq;

  /* swap metadata buffer */
  tmp_buf = strmh->meta_holdbuf;
  strmh->meta_holdbuf = strmh->meta_outbuf;
  strmh->meta_outbuf = tmp_buf;
  strmh->meta_hold_bytes = strmh->meta_got_bytes;

  /* Diagnostic: warn if frame is significantly truncated */
  {
    static int frame_count = 0;
    static int trunc_count = 0;
    frame_count++;
    if (strmh->hold_bytes < strmh->cur_ctrl.dwMaxVideoFrameSize) {
      trunc_count++;
      if (frame_count <= 5 || frame_count % 100 == 0) {
        fprintf(stderr, "[libuvc] Frame#%d truncated: %zu / %u bytes (%.1f%%), %d/%d frames truncated\n",
                frame_count, strmh->hold_bytes, strmh->cur_ctrl.dwMaxVideoFrameSize,
                strmh->hold_bytes * 100.0 / strmh->cur_ctrl.dwMaxVideoFrameSize,
                trunc_count, frame_count);
      }
    } else if (frame_count <= 5 || frame_count % 100 == 0) {
      fprintf(stderr, "[libuvc] Frame#%d complete: %zu bytes\n", frame_count, strmh->hold_bytes);
    }
  }

  pthread_cond_broadcast(&strmh->cb_cond);
  pthread_mutex_unlock(&strmh->cb_mutex);

  strmh->seq++;
  strmh->got_bytes = 0;
  strmh->meta_got_bytes = 0;
  strmh->last_scr = 0;
  strmh->pts = 0;
  strmh->bulk_hdr_done = 0;
}

/** @internal
 * @brief Process a payload transfer
 * 
 * Processes stream, places frames into buffer, signals listeners
 * (such as user callback thread and any polling thread) on new frame
 *
 * @param payload Contents of the payload transfer, either a packet (isochronous) or a full
 * transfer (bulk mode)
 * @param payload_len Length of the payload transfer
 */
void _uvc_process_payload(uvc_stream_handle_t *strmh, uint8_t *payload, size_t payload_len) {
  size_t header_len;
  uint8_t header_info;
  size_t data_len;

  /* magic numbers for identifying header packets from some iSight cameras */
  static uint8_t isight_tag[] = {
    0x11, 0x22, 0x33, 0x44,
    0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xfa, 0xce
  };

  /* ignore empty payload transfers */
  if (payload_len == 0)
    return;

  /* ── BULK mode frame-start header validation ──
   * USB BULK transfer boundaries do not align with UVC frame boundaries.
   * After _uvc_swap_buffers, a late transfer containing the previous
   * frame's tail data may arrive.  At got_bytes==0 we expect a fresh
   * UVC payload header (bHeaderLength == 12, no error bit).  If the
   * first byte doesn't match, this is residual data — discard it.
   * Isochronous and iSight modes are unaffected. */
  if (strmh->is_bulk && !strmh->devh->is_isight
      && strmh->got_bytes == 0 && !strmh->bulk_hdr_done) {
    if (payload_len < 2 || payload[0] != 12 || (payload[1] & 0x40)) {
      UVC_DEBUG("BULK: discarding non-header xfer at frame start "
                "(len=%zu, b0=0x%02x, b1=0x%02x)",
                payload_len, payload[0], payload_len >= 2 ? payload[1] : 0);
      return;
    }
  }

  /* ── BULK mode: after the frame header has been consumed, all subsequent
   *     transfers for the same frame are raw video data without a header.
   *
   *     SAFETY: if a new frame header arrives before the current frame is
   *     complete (got_bytes < dwMaxVideoFrameSize), the old frame's remaining
   *     transfers were lost (USB bandwidth / FPGA back-pressure).  Detect this
   *     by checking whether the incoming payload starts with a valid UVC header
   *     whose FID bit flipped.  If so, publish the truncated old frame and
   *     fall through to normal header parsing for the new frame. */
  if (strmh->is_bulk && (strmh->got_bytes > 0 || strmh->bulk_hdr_done)) {

    if (strmh->got_bytes > 0 && strmh->got_bytes < strmh->cur_ctrl.dwMaxVideoFrameSize
        && payload_len >= 2
        && payload[0] == 12                           /* bHeaderLength */
        && !(payload[1] & 0x40)                       /* no error bit  */
        && (payload[1] & 0x80)                        /* EOH set       */
        && (payload[1] & 1) != strmh->fid) {          /* FID flipped   */
      /* New frame header detected mid-stream — publish truncated old
       * frame so it can be logged as a drop, then process new header. */
      fprintf(stderr, "[libuvc] BULK: new frame header at %zu/%u bytes (xfer_len=%zu, seq=%u) "
              "— FPGA started new frame, old frame truncated\n",
              strmh->got_bytes, strmh->cur_ctrl.dwMaxVideoFrameSize,
              payload_len, strmh->seq);
      _uvc_swap_buffers(strmh);
      strmh->bulk_hdr_done = 0;
      /* fall through to normal header parsing */
    } else {
      header_len = 0;
      data_len = payload_len;
      strmh->bulk_hdr_done = 1;  /* stays set until frame swap */
      header_info = 0;
      goto copy_data;
    }
  }

  /* Certain iSight cameras have strange behavior: They send header
   * information in a packet with no image data, and then the following
   * packets have only image data, with no more headers until the next frame.
   *
   * The iSight header: len(1), flags(1 or 2), 0x11223344(4),
   * 0xdeadbeefdeadface(8), ??(16)
   */

  if (strmh->devh->is_isight &&
      (payload_len < 14 || memcmp(isight_tag, payload + 2, sizeof(isight_tag))) &&
      (payload_len < 15 || memcmp(isight_tag, payload + 3, sizeof(isight_tag)))) {
    /* The payload transfer doesn't have any iSight magic, so it's all image data */
    header_len = 0;
    data_len = payload_len;
  } else {
    header_len = payload[0];

    if (header_len > payload_len) {
      UVC_DEBUG("bogus packet: actual_len=%zd, header_len=%zd\n", payload_len, header_len);
      return;
    }

    if (strmh->devh->is_isight)
      data_len = 0;
    else {
      data_len = payload_len - header_len;
      /* BULK mode: detect header-only transfer.
       * When the device sends the UVC payload header in a separate
       * USB bulk transfer, the next transfer will be raw video data
       * without a header.  Mark this so the next call skips header
       * parsing. */
      if (data_len == 0 && header_len > 0)
        strmh->bulk_hdr_done = 1;
    }
  }

  if (header_len < 2) {
    header_info = 0;
  } else {
    /** @todo we should be checking the end-of-header bit */
    size_t variable_offset = 2;

    header_info = payload[1];

    if (header_info & 0x40) {
      UVC_DEBUG("bad packet: error bit set");
      return;
    }

    if (strmh->fid != (header_info & 1) && strmh->got_bytes != 0) {
      /* The frame ID bit was flipped, but we have image data sitting
         around from prior transfers. This means the camera didn't send
         an EOF for the last transfer of the previous frame. */
      _uvc_swap_buffers(strmh);
    }

    strmh->fid = header_info & 1;

    if (header_info & (1 << 2)) {
      strmh->pts = DW_TO_INT(payload + variable_offset);
      variable_offset += 4;
    }

    if (header_info & (1 << 3)) {
      /** @todo read the SOF token counter */
      strmh->last_scr = DW_TO_INT(payload + variable_offset);
      variable_offset += 6;
    }

    if (header_len > variable_offset) {
        // Metadata is attached to header
        size_t meta_len = header_len - variable_offset;
        if (strmh->meta_got_bytes + meta_len > LIBUVC_XFER_META_BUF_SIZE)
          meta_len = LIBUVC_XFER_META_BUF_SIZE - strmh->meta_got_bytes; /* Avoid overflow. */
        memcpy(strmh->meta_outbuf + strmh->meta_got_bytes, payload + variable_offset, meta_len);
        strmh->meta_got_bytes += meta_len;
    }
  }

copy_data:
  if (data_len > 0) {
    if (strmh->got_bytes + data_len > strmh->cur_ctrl.dwMaxVideoFrameSize)
      data_len = strmh->cur_ctrl.dwMaxVideoFrameSize - strmh->got_bytes; /* Avoid overflow. */
    memcpy(strmh->outbuf + strmh->got_bytes, payload + header_len, data_len);
    strmh->got_bytes += data_len;
    /* Publish frame when data is complete.  In BULK mode the UVC payload
     * header always has EOF set, but the USB driver may split the physical
     * transfer — wait for got_bytes to reach dwMaxVideoFrameSize before
     * publishing, otherwise a late tail transfer would be discarded by the
     * frame-start guard above.  Isochronous mode uses EOF as-is. */
    if (strmh->got_bytes == strmh->cur_ctrl.dwMaxVideoFrameSize
        || ((header_info & (1 << 1)) && !strmh->is_bulk)) {
      _uvc_swap_buffers(strmh);
    }
  }
}

/** @internal
 * @brief Stream transfer callback
 *
 * Processes stream, places frames into buffer, signals listeners
 * (such as user callback thread and any polling thread) on new frame
 *
 * @param transfer Active transfer
 */
void LIBUSB_CALL _uvc_stream_callback(struct libusb_transfer *transfer) {
  uvc_stream_handle_t *strmh = transfer->user_data;

  int resubmit = 1;

  switch (transfer->status) {
  case LIBUSB_TRANSFER_COMPLETED:
    if (transfer->num_iso_packets == 0) {
      /* This is a bulk mode transfer, so it just has one payload transfer */
      _uvc_process_payload(strmh, transfer->buffer, transfer->actual_length);
    } else {
      /* This is an isochronous mode transfer, so each packet has a payload transfer */
      int packet_id;

      for (packet_id = 0; packet_id < transfer->num_iso_packets; ++packet_id) {
        uint8_t *pktbuf;
        struct libusb_iso_packet_descriptor *pkt;

        pkt = transfer->iso_packet_desc + packet_id;

        if (pkt->status != 0) {
          UVC_DEBUG("bad packet (isochronous transfer); status: %d", pkt->status);
          continue;
        }

        pktbuf = libusb_get_iso_packet_buffer_simple(transfer, packet_id);

        _uvc_process_payload(strmh, pktbuf, pkt->actual_length);

      }
    }
    break;
  case LIBUSB_TRANSFER_CANCELLED: 
  case LIBUSB_TRANSFER_ERROR:
  case LIBUSB_TRANSFER_NO_DEVICE: {
    int i;
    UVC_DEBUG("not retrying transfer, status = %d", transfer->status);
    pthread_mutex_lock(&strmh->cb_mutex);

    /* Mark transfer as deleted. */
    for(i=0; i < LIBUVC_NUM_TRANSFER_BUFS; i++) {
      if(strmh->transfers[i] == transfer) {
        UVC_DEBUG("Freeing transfer %d (%p)", i, transfer);
        free(transfer->buffer);
        libusb_free_transfer(transfer);
        strmh->transfers[i] = NULL;
        break;
      }
    }
    if(i == LIBUVC_NUM_TRANSFER_BUFS ) {
      UVC_DEBUG("transfer %p not found; not freeing!", transfer);
    }

    resubmit = 0;

    pthread_cond_broadcast(&strmh->cb_cond);
    pthread_mutex_unlock(&strmh->cb_mutex);

    break;
  }
  case LIBUSB_TRANSFER_TIMED_OUT:
  case LIBUSB_TRANSFER_STALL:
  case LIBUSB_TRANSFER_OVERFLOW:
    UVC_DEBUG("retrying transfer, status = %d", transfer->status);
    break;
  }
  
  if ( resubmit ) {
    if ( strmh->running ) {
      int libusbRet = libusb_submit_transfer(transfer);
      if (libusbRet < 0)
      {
        int i;
        pthread_mutex_lock(&strmh->cb_mutex);

        /* Mark transfer as deleted. */
        for (i = 0; i < LIBUVC_NUM_TRANSFER_BUFS; i++) {
          if (strmh->transfers[i] == transfer) {
            UVC_DEBUG("Freeing failed transfer %d (%p)", i, transfer);
            free(transfer->buffer);
            libusb_free_transfer(transfer);
            strmh->transfers[i] = NULL;
            break;
          }
        }
        if (i == LIBUVC_NUM_TRANSFER_BUFS) {
          UVC_DEBUG("failed transfer %p not found; not freeing!", transfer);
        }

        pthread_cond_broadcast(&strmh->cb_cond);
        pthread_mutex_unlock(&strmh->cb_mutex);
      }
    } else {
      int i;
      pthread_mutex_lock(&strmh->cb_mutex);

      /* Mark transfer as deleted. */
      for(i=0; i < LIBUVC_NUM_TRANSFER_BUFS; i++) {
        if(strmh->transfers[i] == transfer) {
          UVC_DEBUG("Freeing orphan transfer %d (%p)", i, transfer);
          free(transfer->buffer);
          libusb_free_transfer(transfer);
          strmh->transfers[i] = NULL;
          break;
        }
      }
      if(i == LIBUVC_NUM_TRANSFER_BUFS ) {
        UVC_DEBUG("orphan transfer %p not found; not freeing!", transfer);
      }

      pthread_cond_broadcast(&strmh->cb_cond);
      pthread_mutex_unlock(&strmh->cb_mutex);
    }
  }
}

/** Begin streaming video from the camera into the callback function.
 * @ingroup streaming
 *
 * @param devh UVC device
 * @param ctrl Control block, processed using {uvc_probe_stream_ctrl} or
 *             {uvc_get_stream_ctrl_format_size}
 * @param cb   User callback function. See {uvc_frame_callback_t} for restrictions.
 * @param flags Stream setup flags, currently undefined. Set this to zero. The lower bit
 * is reserved for backward compatibility.
 */
uvc_error_t uvc_start_streaming(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl,
    uvc_frame_callback_t *cb,
    void *user_ptr,
    uint8_t flags
) {
  uvc_error_t ret;
  uvc_stream_handle_t *strmh;

  ret = uvc_stream_open_ctrl(devh, &strmh, ctrl);
  if (ret != UVC_SUCCESS)
    return ret;

  ret = uvc_stream_start(strmh, cb, user_ptr, flags);
  if (ret != UVC_SUCCESS) {
    uvc_stream_close(strmh);
    return ret;
  }

  return UVC_SUCCESS;
}

/** Begin streaming video from the camera into the callback function.
 * @ingroup streaming
 *
 * @deprecated The stream type (bulk vs. isochronous) will be determined by the
 * type of interface associated with the uvc_stream_ctrl_t parameter, regardless
 * of whether the caller requests isochronous streaming. Please switch to
 * uvc_start_streaming().
 *
 * @param devh UVC device
 * @param ctrl Control block, processed using {uvc_probe_stream_ctrl} or
 *             {uvc_get_stream_ctrl_format_size}
 * @param cb   User callback function. See {uvc_frame_callback_t} for restrictions.
 */
uvc_error_t uvc_start_iso_streaming(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl,
    uvc_frame_callback_t *cb,
    void *user_ptr
) {
  return uvc_start_streaming(devh, ctrl, cb, user_ptr, 0);
}

static uvc_stream_handle_t *_uvc_get_stream_by_interface(uvc_device_handle_t *devh, int interface_idx) {
  uvc_stream_handle_t *strmh;

  DL_FOREACH(devh->streams, strmh) {
    if (strmh->stream_if->bInterfaceNumber == interface_idx)
      return strmh;
  }

  return NULL;
}

static uvc_streaming_interface_t *_uvc_get_stream_if(uvc_device_handle_t *devh, int interface_idx) {
  uvc_streaming_interface_t *stream_if;

  DL_FOREACH(devh->info->stream_ifs, stream_if) {
    if (stream_if->bInterfaceNumber == interface_idx)
      return stream_if;
  }
  
  return NULL;
}

/** Open a new video stream.
 * @ingroup streaming
 *
 * @param devh UVC device
 * @param ctrl Control block, processed using {uvc_probe_stream_ctrl} or
 *             {uvc_get_stream_ctrl_format_size}
 */
uvc_error_t uvc_stream_open_ctrl(uvc_device_handle_t *devh, uvc_stream_handle_t **strmhp, uvc_stream_ctrl_t *ctrl) {
  /* Chosen frame and format descriptors */
  uvc_stream_handle_t *strmh = NULL;
  uvc_streaming_interface_t *stream_if;
  uvc_error_t ret;

  UVC_ENTER();

  if (_uvc_get_stream_by_interface(devh, ctrl->bInterfaceNumber) != NULL) {
    ret = UVC_ERROR_BUSY; /* Stream is already opened */
    goto fail;
  }

  stream_if = _uvc_get_stream_if(devh, ctrl->bInterfaceNumber);
  if (!stream_if) {
    ret = UVC_ERROR_INVALID_PARAM;
    goto fail;
  }

  strmh = calloc(1, sizeof(*strmh));
  if (!strmh) {
    ret = UVC_ERROR_NO_MEM;
    goto fail;
  }
  strmh->devh = devh;
  strmh->stream_if = stream_if;
  strmh->frame.library_owns_data = 1;

  ret = uvc_claim_if(strmh->devh, strmh->stream_if->bInterfaceNumber);
  if (ret != UVC_SUCCESS)
    goto fail;

  ret = uvc_stream_ctrl(strmh, ctrl);
  if (ret != UVC_SUCCESS)
    goto fail;

  // Set up the streaming status and data space
  strmh->running = 0;

  strmh->outbuf = malloc( ctrl->dwMaxVideoFrameSize );
  strmh->holdbuf = malloc( ctrl->dwMaxVideoFrameSize );

  strmh->meta_outbuf = malloc( LIBUVC_XFER_META_BUF_SIZE );
  strmh->meta_holdbuf = malloc( LIBUVC_XFER_META_BUF_SIZE );
   
  pthread_mutex_init(&strmh->cb_mutex, NULL);
  pthread_cond_init(&strmh->cb_cond, NULL);

  DL_APPEND(devh->streams, strmh);

  *strmhp = strmh;

  UVC_EXIT(0);
  return UVC_SUCCESS;

fail:
  if(strmh)
    free(strmh);
  UVC_EXIT(ret);
  return ret;
}

/** Begin streaming video from the stream into the callback function.
 * @ingroup streaming
 *
 * @param strmh UVC stream
 * @param cb   User callback function. See {uvc_frame_callback_t} for restrictions.
 * @param flags Stream setup flags, currently undefined. Set this to zero. The lower bit
 * is reserved for backward compatibility.
 */
uvc_error_t uvc_stream_start(
    uvc_stream_handle_t *strmh,
    uvc_frame_callback_t *cb,
    void *user_ptr,
    uint8_t flags
) {
  /* USB interface we'll be using */
  const struct libusb_interface *interface;
  int interface_id;
  char isochronous;
  uvc_frame_desc_t *frame_desc;
  uvc_format_desc_t *format_desc;
  uvc_stream_ctrl_t *ctrl;
  uvc_error_t ret;
  /* Total amount of data per transfer */
  size_t total_transfer_size = 0;
  struct libusb_transfer *transfer;
  int transfer_id;
  int submitted_transfers = 0;

  ctrl = &strmh->cur_ctrl;

  UVC_ENTER();

  if (strmh->running) {
    UVC_EXIT(UVC_ERROR_BUSY);
    return UVC_ERROR_BUSY;
  }

  strmh->running = 1;
  strmh->seq = 1;
  strmh->fid = 0;
  strmh->bulk_hdr_done = 0;
  strmh->pts = 0;
  strmh->last_scr = 0;

  frame_desc = uvc_find_frame_desc_stream(strmh, ctrl->bFormatIndex, ctrl->bFrameIndex);
  if (!frame_desc) {
    ret = UVC_ERROR_INVALID_PARAM;
    goto fail;
  }
  format_desc = frame_desc->parent;

  strmh->frame_format = uvc_frame_format_for_guid(format_desc->guidFormat);
  if (strmh->frame_format == UVC_FRAME_FORMAT_UNKNOWN) {
    ret = UVC_ERROR_NOT_SUPPORTED;
    goto fail;
  }

  _uvc_clear_stream_endpoint(strmh, "before-start");

  // Get the interface that provides the chosen format and frame configuration
  interface_id = strmh->stream_if->bInterfaceNumber;
  interface = &strmh->devh->info->config->interface[interface_id];

  /* A VS interface uses isochronous transfers iff it has multiple altsettings.
   * (UVC 1.5: 2.4.3. VideoStreaming Interface) */
  isochronous = interface->num_altsetting > 1;
  strmh->is_bulk = !isochronous;

  if (isochronous) {
    /* For isochronous streaming, we choose an appropriate altsetting for the endpoint
     * and set up several transfers */
    const struct libusb_interface_descriptor *altsetting = 0;
    const struct libusb_endpoint_descriptor *endpoint = 0;
    /* The greatest number of bytes that the device might provide, per packet, in this
     * configuration */
    size_t config_bytes_per_packet;
    /* Number of packets per transfer */
    size_t packets_per_transfer = 0;
    /* Size of packet transferable from the chosen endpoint */
    size_t endpoint_bytes_per_packet = 0;
    /* Index of the altsetting */
    int alt_idx, ep_idx;
    
    config_bytes_per_packet = strmh->cur_ctrl.dwMaxPayloadTransferSize;

    /* Go through the altsettings and find one whose packets are at least
     * as big as our format's maximum per-packet usage. Assume that the
     * packet sizes are increasing. */
    for (alt_idx = 0; alt_idx < interface->num_altsetting; alt_idx++) {
      altsetting = interface->altsetting + alt_idx;
      endpoint_bytes_per_packet = 0;

      /* Find the endpoint with the number specified in the VS header */
      for (ep_idx = 0; ep_idx < altsetting->bNumEndpoints; ep_idx++) {
        endpoint = altsetting->endpoint + ep_idx;

        struct libusb_ss_endpoint_companion_descriptor *ep_comp = 0;
        libusb_get_ss_endpoint_companion_descriptor(NULL, endpoint, &ep_comp);
        if (ep_comp)
        {
          endpoint_bytes_per_packet = ep_comp->wBytesPerInterval;
          libusb_free_ss_endpoint_companion_descriptor(ep_comp);
          break;
        }
        else
        {
          if (endpoint->bEndpointAddress == format_desc->parent->bEndpointAddress) {
              endpoint_bytes_per_packet = endpoint->wMaxPacketSize;
            // wMaxPacketSize: [unused:2 (multiplier-1):3 size:11]
            endpoint_bytes_per_packet = (endpoint_bytes_per_packet & 0x07ff) *
              (((endpoint_bytes_per_packet >> 11) & 3) + 1);
            break;
          }
        }
      }

      if (endpoint_bytes_per_packet >= config_bytes_per_packet) {
        /* Transfers will be at most one frame long: Divide the maximum frame size
         * by the size of the endpoint and round up */
        packets_per_transfer = (ctrl->dwMaxVideoFrameSize +
                                endpoint_bytes_per_packet - 1) / endpoint_bytes_per_packet;

        /* High-bandwidth cameras need many packets per transfer.
         * Original limit of 32 was too low for 10MB+ frames at 60FPS,
         * causing severe packet loss. Raised to 256 for 2560x2048 Gray16. */
        if (packets_per_transfer > 256)
          packets_per_transfer = 256;
        
        total_transfer_size = packets_per_transfer * endpoint_bytes_per_packet;
        break;
      }
    }

    /* If we searched through all the altsettings and found nothing usable */
    if (alt_idx == interface->num_altsetting) {
      ret = UVC_ERROR_INVALID_MODE;
      goto fail;
    }

    /* Select the altsetting */
    ret = libusb_set_interface_alt_setting(strmh->devh->usb_devh,
                                           altsetting->bInterfaceNumber,
                                           altsetting->bAlternateSetting);
    if (ret != UVC_SUCCESS) {
      UVC_DEBUG("libusb_set_interface_alt_setting failed");
      goto fail;
    }

    /* Set up the transfers */
    for (transfer_id = 0; transfer_id < LIBUVC_NUM_TRANSFER_BUFS; ++transfer_id) {
      transfer = libusb_alloc_transfer(packets_per_transfer);
      strmh->transfers[transfer_id] = transfer;      
      strmh->transfer_bufs[transfer_id] = malloc(total_transfer_size);

      libusb_fill_iso_transfer(
        transfer, strmh->devh->usb_devh, format_desc->parent->bEndpointAddress,
        strmh->transfer_bufs[transfer_id],
        total_transfer_size, packets_per_transfer, _uvc_stream_callback, (void*) strmh, 5000);

      libusb_set_iso_packet_lengths(transfer, endpoint_bytes_per_packet);
    }

    /* ── 诊断日志：打印流启动参数 ── */
    fprintf(stderr,
      "[libuvc] ========== Stream Start ==========\n"
      "[libuvc]   Frame size (dwMaxVideoFrameSize):  %u bytes (%.1f KB)\n"
      "[libuvc]   Payload limit (dwMaxPayloadTransferSize): %u bytes (%.1f KB)\n"
      "[libuvc]   Endpoint bytes per packet:          %zu bytes (%.1f KB)\n"
      "[libuvc]   Packets per transfer:               %zu\n"
      "[libuvc]   Total per transfer:                 %zu bytes (%.1f KB)\n"
      "[libuvc]   Transfer buffers:                   %d\n"
      "[libuvc]   Buffer pool total:                  %.1f MB\n"
      "[libuvc]   Frame / buffer pool:                %.1f frames\n"
      "[libuvc] ===================================\n",
      ctrl->dwMaxVideoFrameSize,
      ctrl->dwMaxVideoFrameSize / 1024.0,
      ctrl->dwMaxPayloadTransferSize,
      ctrl->dwMaxPayloadTransferSize / 1024.0,
      endpoint_bytes_per_packet,
      endpoint_bytes_per_packet / 1024.0,
      packets_per_transfer,
      total_transfer_size,
      total_transfer_size / 1024.0,
      LIBUVC_NUM_TRANSFER_BUFS,
      total_transfer_size * LIBUVC_NUM_TRANSFER_BUFS / (1024.0 * 1024.0),
      (total_transfer_size * LIBUVC_NUM_TRANSFER_BUFS) / (double)ctrl->dwMaxVideoFrameSize
    );
  } else {
    /* ── Bulk 模式传输大小策略 ──
     *
     * 关键发现 (2026-07-14 笔记本 XHCI 实测):
     *   1MB transfer → 10% 丢帧率 (每帧 10 个 transfer 边界 = 10 个丢包点)
     *   10MB transfer → 4% 丢帧率 (每帧 1 个 transfer 边界 = 1 个丢包点)
     *
     * 原因: 每次 bulk transfer 完成 (被 short packet 截断或读满) 后, XHCI 需要
     * 处理完成中断 → libusb 回调 → resubmit, 这段间隙里 FT602 FIFO (32KB) 无人
     * 读取, 若 XHCI 调度抖动 > 0.16ms 则 FIFO 溢出 → FPGA 丢数据 → 开新帧。
     *
     * 结论: transfer 越大, 边界越少, 丢包越少。使用整帧 transfer (dwMaxPayloadTransferSize)
     * 配合大量 buffer, 让 XHCI 调度队列始终有 pending transfer 减少间隙。
     *
     * 可选: LIBUVC_BULK_XFER_SIZE_OVERRIDE 宏可强制指定 transfer 大小 (不推荐)
     */
#ifdef LIBUVC_BULK_XFER_SIZE_OVERRIDE
    size_t bulk_xfer_size = LIBUVC_BULK_XFER_SIZE_OVERRIDE;
#else
    size_t bulk_xfer_size = strmh->cur_ctrl.dwMaxPayloadTransferSize;
#endif

    _uvc_set_stream_interface_alt0(strmh, "bulk-before-start");
    _uvc_clear_stream_endpoint(strmh, "bulk-before-start");

    for (transfer_id = 0; transfer_id < LIBUVC_NUM_TRANSFER_BUFS;
        ++transfer_id) {
      transfer = libusb_alloc_transfer(0);
      strmh->transfers[transfer_id] = transfer;
      strmh->transfer_bufs[transfer_id] = malloc(bulk_xfer_size);
      libusb_fill_bulk_transfer(transfer, strmh->devh->usb_devh,
          format_desc->parent->bEndpointAddress,
          strmh->transfer_bufs[transfer_id],
          bulk_xfer_size, _uvc_stream_callback,
          (void*) strmh, 5000 );
    }

    /* ── 诊断日志：Bulk 模式参数 ── */
    {
      double pool_mb = bulk_xfer_size * LIBUVC_NUM_TRANSFER_BUFS / (1024.0 * 1024.0);
      double xfers_per_frame = ctrl->dwMaxVideoFrameSize > 0
        ? (double)ctrl->dwMaxVideoFrameSize / bulk_xfer_size : 0;
      fprintf(stderr,
        "[libuvc] ========== Stream Start (BULK) ==========\n"
        "[libuvc]   Frame size:        %u bytes (%.1f KB)\n"
        "[libuvc]   Device payload:    %u bytes (%.1f KB)\n"
        "[libuvc]   Actual xfer size:  %zu bytes (%.1f KB)\n"
        "[libuvc]   Transfer buffers:  %d\n"
        "[libuvc]   Buffer pool:       %.1f MB\n"
        "[libuvc]   Xfers per frame:   %.1f\n"
        "[libuvc] =========================================\n",
        ctrl->dwMaxVideoFrameSize,
        ctrl->dwMaxVideoFrameSize / 1024.0,
        ctrl->dwMaxPayloadTransferSize,
        ctrl->dwMaxPayloadTransferSize / 1024.0,
        bulk_xfer_size,
        bulk_xfer_size / 1024.0,
        LIBUVC_NUM_TRANSFER_BUFS,
        pool_mb,
        xfers_per_frame
      );
    }
  }

  strmh->user_cb = cb;
  strmh->user_ptr = user_ptr;

  /* If the user wants it, set up a thread that calls the user's function
   * with the contents of each frame.
   */
  if (cb) {
    pthread_create(&strmh->cb_thread, NULL, _uvc_user_caller, (void*) strmh);
  }

  for (transfer_id = 0; transfer_id < LIBUVC_NUM_TRANSFER_BUFS;
      transfer_id++) {
    ret = libusb_submit_transfer(strmh->transfers[transfer_id]);
    if (ret != UVC_SUCCESS) {
      UVC_DEBUG("libusb_submit_transfer failed: %d",ret);
      break;
    }
    submitted_transfers++;
  }

  if (ret != UVC_SUCCESS) {
    int i;

    strmh->running = 0;

    pthread_mutex_lock(&strmh->cb_mutex);

    for (i = 0; i < submitted_transfers; i++) {
      if (strmh->transfers[i] != NULL)
        libusb_cancel_transfer(strmh->transfers[i]);
    }

    for (i = submitted_transfers; i < LIBUVC_NUM_TRANSFER_BUFS; i++) {
      if (strmh->transfers[i] != NULL) {
        free(strmh->transfers[i]->buffer);
        libusb_free_transfer(strmh->transfers[i]);
        strmh->transfers[i] = NULL;
      }
    }

    do {
      for (i = 0; i < submitted_transfers; i++) {
        if (strmh->transfers[i] != NULL)
          break;
      }
      if (i == submitted_transfers)
        break;
      pthread_cond_wait(&strmh->cb_cond, &strmh->cb_mutex);
    } while (1);

    pthread_cond_broadcast(&strmh->cb_cond);
    pthread_mutex_unlock(&strmh->cb_mutex);

    if (strmh->user_cb)
      pthread_join(strmh->cb_thread, NULL);

    UVC_EXIT(ret);
    return ret;
  }

  UVC_EXIT(ret);
  return ret;
fail:
  strmh->running = 0;
  UVC_EXIT(ret);
  return ret;
}

/** Begin streaming video from the stream into the callback function.
 * @ingroup streaming
 *
 * @deprecated The stream type (bulk vs. isochronous) will be determined by the
 * type of interface associated with the uvc_stream_ctrl_t parameter, regardless
 * of whether the caller requests isochronous streaming. Please switch to
 * uvc_stream_start().
 *
 * @param strmh UVC stream
 * @param cb   User callback function. See {uvc_frame_callback_t} for restrictions.
 */
uvc_error_t uvc_stream_start_iso(
    uvc_stream_handle_t *strmh,
    uvc_frame_callback_t *cb,
    void *user_ptr
) {
  return uvc_stream_start(strmh, cb, user_ptr, 0);
}

/** @internal
 * @brief User callback runner thread
 * @note There should be at most one of these per currently streaming device
 * @param arg Device handle
 */
void *_uvc_user_caller(void *arg) {
  uvc_stream_handle_t *strmh = (uvc_stream_handle_t *) arg;

  uint32_t last_seq = 0;

  do {
    pthread_mutex_lock(&strmh->cb_mutex);

    while (strmh->running && last_seq == strmh->hold_seq) {
      pthread_cond_wait(&strmh->cb_cond, &strmh->cb_mutex);
    }

    if (!strmh->running) {
      pthread_mutex_unlock(&strmh->cb_mutex);
      break;
    }
    
    last_seq = strmh->hold_seq;
    _uvc_populate_frame(strmh);
    
    pthread_mutex_unlock(&strmh->cb_mutex);
    
    strmh->user_cb(&strmh->frame, strmh->user_ptr);
  } while(1);

  return NULL; // return value ignored
}

/** @internal
 * @brief Populate the fields of a frame to be handed to user code
 * must be called with stream cb lock held!
 */
void _uvc_populate_frame(uvc_stream_handle_t *strmh) {
  uvc_frame_t *frame = &strmh->frame;
  uvc_frame_desc_t *frame_desc;

  /** @todo this stuff that hits the main config cache should really happen
   * in start() so that only one thread hits these data. all of this stuff
   * is going to be reopen_on_change anyway
   */

  frame_desc = uvc_find_frame_desc(strmh->devh, strmh->cur_ctrl.bFormatIndex,
				   strmh->cur_ctrl.bFrameIndex);

  frame->frame_format = strmh->frame_format;
  
  frame->width = frame_desc->wWidth;
  frame->height = frame_desc->wHeight;
  
  switch (frame->frame_format) {
  case UVC_FRAME_FORMAT_BGR:
    frame->step = frame->width * 3;
    break;
  case UVC_FRAME_FORMAT_YUYV:
    frame->step = frame->width * 2;
    break;
  case UVC_FRAME_FORMAT_NV12:
    frame->step = frame->width;
    break;
  case UVC_FRAME_FORMAT_P010:
    frame->step = frame->width * 2;
        break;
  case UVC_FRAME_FORMAT_MJPEG:
    frame->step = 0;
    break;
  case UVC_FRAME_FORMAT_H264:
    frame->step = 0;
    break;
  default:
    frame->step = 0;
    break;
  }

  frame->sequence = strmh->hold_seq;
  frame->capture_time_finished = strmh->capture_time_finished;

  /* copy the image data from the hold buffer to the frame (unnecessary extra buf?) */
  if (frame->data_bytes < strmh->hold_bytes) {
    frame->data = realloc(frame->data, strmh->hold_bytes);
  }
  frame->data_bytes = strmh->hold_bytes;
  memcpy(frame->data, strmh->holdbuf, frame->data_bytes);

  if (strmh->meta_hold_bytes > 0)
  {
      if (frame->metadata_bytes < strmh->meta_hold_bytes)
      {
          frame->metadata = realloc(frame->metadata, strmh->meta_hold_bytes);
      }
      frame->metadata_bytes = strmh->meta_hold_bytes;
      memcpy(frame->metadata, strmh->meta_holdbuf, frame->metadata_bytes);
  }
}

/** Poll for a frame
 * @ingroup streaming
 *
 * @param devh UVC device
 * @param[out] frame Location to store pointer to captured frame (NULL on error)
 * @param timeout_us >0: Wait at most N microseconds; 0: Wait indefinitely; -1: return immediately
 */
uvc_error_t uvc_stream_get_frame(uvc_stream_handle_t *strmh,
			  uvc_frame_t **frame,
			  int32_t timeout_us) {
  time_t add_secs;
  time_t add_nsecs;
  struct timespec ts;

  if (!strmh->running)
    return UVC_ERROR_INVALID_PARAM;

  if (strmh->user_cb)
    return UVC_ERROR_CALLBACK_EXISTS;

  pthread_mutex_lock(&strmh->cb_mutex);

  if (strmh->last_polled_seq < strmh->hold_seq) {
    _uvc_populate_frame(strmh);
    *frame = &strmh->frame;
    strmh->last_polled_seq = strmh->hold_seq;
  } else if (timeout_us != -1) {
    if (timeout_us == 0) {
      pthread_cond_wait(&strmh->cb_cond, &strmh->cb_mutex);
    } else {
      add_secs = timeout_us / 1000000;
      add_nsecs = (timeout_us % 1000000) * 1000;
      ts.tv_sec = 0;
      ts.tv_nsec = 0;

#if _POSIX_TIMERS > 0
      clock_gettime(CLOCK_REALTIME, &ts);
#else
      struct timeval tv;
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec;
      ts.tv_nsec = tv.tv_usec * 1000;
#endif

      ts.tv_sec += add_secs;
      ts.tv_nsec += add_nsecs;

      /* pthread_cond_timedwait FAILS with EINVAL if ts.tv_nsec > 1000000000 (1 billion)
       * Since we are just adding values to the timespec, we have to increment the seconds if nanoseconds is greater than 1 billion,
       * and then re-adjust the nanoseconds in the correct range.
       * */
      ts.tv_sec += ts.tv_nsec / 1000000000;
      ts.tv_nsec = ts.tv_nsec % 1000000000;

      int err = pthread_cond_timedwait(&strmh->cb_cond, &strmh->cb_mutex, &ts);

      //TODO: How should we handle EINVAL?
      if (err) {
        *frame = NULL;
        pthread_mutex_unlock(&strmh->cb_mutex);
        return err == ETIMEDOUT ? UVC_ERROR_TIMEOUT : UVC_ERROR_OTHER;
      }
    }
    
    if (strmh->last_polled_seq < strmh->hold_seq) {
      _uvc_populate_frame(strmh);
      *frame = &strmh->frame;
      strmh->last_polled_seq = strmh->hold_seq;
    } else {
      *frame = NULL;
    }
  } else {
    *frame = NULL;
  }

  pthread_mutex_unlock(&strmh->cb_mutex);

  return UVC_SUCCESS;
}

/** @brief Stop streaming video
 * @ingroup streaming
 *
 * Closes all streams, ends threads and cancels pollers
 *
 * @param devh UVC device
 */
void uvc_stop_streaming(uvc_device_handle_t *devh) {
  uvc_stream_handle_t *strmh, *strmh_tmp;

  DL_FOREACH_SAFE(devh->streams, strmh, strmh_tmp) {
    uvc_stream_close(strmh);
  }
}

/** @brief Stop stream.
 * @ingroup streaming
 *
 * Stops stream, ends threads and cancels pollers
 *
 * @param devh UVC device
 */
uvc_error_t uvc_stream_stop(uvc_stream_handle_t *strmh) {
  int i;

  if (!strmh->running)
    return UVC_ERROR_INVALID_PARAM;

  strmh->running = 0;

  pthread_mutex_lock(&strmh->cb_mutex);

  /* Attempt to cancel any running transfers, we can't free them just yet because they aren't
   *   necessarily completed but they will be free'd in _uvc_stream_callback().
   */
  for(i=0; i < LIBUVC_NUM_TRANSFER_BUFS; i++) {
    if(strmh->transfers[i] != NULL)
      libusb_cancel_transfer(strmh->transfers[i]);
  }

  /* Wait for transfers to complete/cancel */
  do {
    for(i=0; i < LIBUVC_NUM_TRANSFER_BUFS; i++) {
      if(strmh->transfers[i] != NULL)
        break;
    }
    if(i == LIBUVC_NUM_TRANSFER_BUFS )
      break;
    pthread_cond_wait(&strmh->cb_cond, &strmh->cb_mutex);
  } while(1);
  // Kick the user thread awake
  pthread_cond_broadcast(&strmh->cb_cond);
  pthread_mutex_unlock(&strmh->cb_mutex);

  _uvc_clear_stream_endpoint(strmh, "after-stop");

  /** @todo stop the actual stream, camera side? */

  if (strmh->user_cb) {
    /* wait for the thread to stop (triggered by
     * LIBUSB_TRANSFER_CANCELLED transfer) */
    pthread_join(strmh->cb_thread, NULL);
  }

  return UVC_SUCCESS;
}

/** @brief Close stream.
 * @ingroup streaming
 *
 * Closes stream, frees handle and all streaming resources.
 *
 * @param strmh UVC stream handle
 */
void uvc_stream_close(uvc_stream_handle_t *strmh) {
  if (strmh->running)
    uvc_stream_stop(strmh);

  uvc_release_if(strmh->devh, strmh->stream_if->bInterfaceNumber);

  if (strmh->frame.data)
    free(strmh->frame.data);

  free(strmh->outbuf);
  free(strmh->holdbuf);

  free(strmh->meta_outbuf);
  free(strmh->meta_holdbuf);

  pthread_cond_destroy(&strmh->cb_cond);
  pthread_mutex_destroy(&strmh->cb_mutex);

  DL_DELETE(strmh->devh->streams, strmh);
  free(strmh);
}

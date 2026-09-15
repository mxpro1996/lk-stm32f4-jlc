/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "text_protocol.h"
#include <stdio.h>

EfiStatus output_string(struct EfiSimpleTextOutputProtocol *self,
                        uint16_t *string) {
  char buffer[512];
  size_t i = 0;
  while (string[i]) {
    size_t j = 0;
    for (j = 0; j < sizeof(buffer) - 1 && string[i + j]; j++) {
      buffer[j] = string[i + j];
    }
    i += j;
    buffer[j] = 0;

    printf("%s", reinterpret_cast<const char *>(buffer));
  }
  return EFI_STATUS_SUCCESS;
}

namespace {

EfiStatus reset(struct EfiSimpleTextOutputProtocol *self,
                bool extended_verification) {
  return EFI_STATUS_SUCCESS;
}

EfiStatus test_string(struct EfiSimpleTextOutputProtocol *self,
                      uint16_t *string) {
  return EFI_STATUS_SUCCESS;
}

EfiStatus query_mode(struct EfiSimpleTextOutputProtocol *self, size_t mode_num,
                     size_t *cols, size_t *rows) {
  if (mode_num != 0) {
    return EFI_STATUS_UNSUPPORTED;
  }
  if (cols != nullptr) {
    *cols = 80;
  }
  if (rows != nullptr) {
    *rows = 25;
  }
  return EFI_STATUS_SUCCESS;
}

EfiStatus set_mode(struct EfiSimpleTextOutputProtocol *self, size_t mode_num) {
  if (mode_num != 0) {
    return EFI_STATUS_UNSUPPORTED;
  }
  return EFI_STATUS_SUCCESS;
}

// EFI default: light gray text on a black background.
SimpleTextOutputMode console_out_mode = {
    .max_mode = 1,
    .mode = 0,
    .attribute = 0x07,
    .cursor_column = 0,
    .cursor_row = 0,
    .cursor_visible = false,
};

EfiStatus set_attribute(struct EfiSimpleTextOutputProtocol *self,
                        size_t attribute) {
  console_out_mode.attribute = static_cast<int32_t>(attribute);
  return EFI_STATUS_SUCCESS;
}

EfiStatus clear_screen(struct EfiSimpleTextOutputProtocol *self) {
  return EFI_STATUS_SUCCESS;
}

EfiStatus set_cursor_position(struct EfiSimpleTextOutputProtocol *self,
                              size_t col, size_t row) {
  return EFI_STATUS_UNSUPPORTED;
}

EfiStatus enable_cursor(struct EfiSimpleTextOutputProtocol *self,
                        bool visible) {
  return EFI_STATUS_UNSUPPORTED;
}

}  // namespace

EfiSimpleTextOutputProtocol get_text_output_protocol() {
  EfiSimpleTextOutputProtocol console_out = {};
  console_out.reset = reset;
  console_out.output_string = output_string;
  console_out.test_string = test_string;
  console_out.query_mode = query_mode;
  console_out.set_mode = set_mode;
  console_out.set_attribute = set_attribute;
  console_out.clear_screen = clear_screen;
  console_out.set_cursor_position = set_cursor_position;
  console_out.enable_cursor = enable_cursor;
  console_out.mode = &console_out_mode;
  return console_out;
}

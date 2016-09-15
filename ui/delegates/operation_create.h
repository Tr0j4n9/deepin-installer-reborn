// Copyright (c) 2016 Deepin Ltd. All rights reserved.
// Use of this source is governed by General Public License that can be found
// in the LICENSE file.

#ifndef DEEPIN_INSTALLER_REBORN_UI_DELEGATES_OPERATION_CREATE_H
#define DEEPIN_INSTALLER_REBORN_UI_DELEGATES_OPERATION_CREATE_H

#include "ui/delegates/operation.h"

namespace ui {

// To mark new partition is created
class OperationCreate : public Operation{
 public:
  OperationCreate(const partman::Partition& partition_orig,
                  const partman::Partition& partition_new);
};

}  // namespace ui

#endif  // DEEPIN_INSTALLER_REBORN_UI_DELEGATES_OPERATION_CREATE_H
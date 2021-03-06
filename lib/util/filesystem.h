// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_UTIL_FILESYSTEM_H_
#define PERIDOT_LIB_UTIL_FILESYSTEM_H_

namespace modular {

// Sleep until Minfs is mounted.
void WaitForMinfs();

}  // namespace modular

#endif  // PERIDOT_LIB_UTIL_FILESYSTEM_H_

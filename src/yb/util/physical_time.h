// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_UTIL_PHYSICAL_TIME_H
#define YB_UTIL_PHYSICAL_TIME_H

#include "yb/util/result.h"

namespace yb {

using MicrosTime = uint64_t;

struct PhysicalTime {
  MicrosTime time_point;
  MicrosTime max_error;
};

class PhysicalClock {
 public:
  virtual Result<PhysicalTime> Now() = 0;
  virtual ~PhysicalClock() {}
};

typedef std::shared_ptr<PhysicalClock> PhysicalClockPtr;
typedef std::function<PhysicalClockPtr()> PhysicalClockProvider;

// Clock with user controlled return values.
class MockClock : public PhysicalClock {
 public:
  Result<PhysicalTime> Now() override;

  void Set(const PhysicalTime& value);

  // Constructs PhysicalClockPtr from this object.
  PhysicalClockPtr AsClock();

  // Constructs PhysicalClockProvider from this object.
  PhysicalClockProvider AsProvider();

 private:
  // Set by calls to SetMockClockWallTimeForTests().
  // For testing purposes only.
  std::atomic<PhysicalTime> value_{{0, 0}};
};

const PhysicalClockPtr& WallClock();

} // namespace yb

#endif // YB_UTIL_PHYSICAL_TIME_H

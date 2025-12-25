// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file at_lens.h
/// @brief Scheme 3: Using lager::lenses::at with Value's container interface.
///
/// This is the simplest approach: by implementing at(), set(), count(),
/// and size() on Value, we can use lager::lenses::at directly without
/// any custom Path-based lens construction.
///
/// Key insight: lager::lenses::at calls whole.at(key) and whole.set(key, val)
/// Our Value now supports these methods!

#pragma once

#include <lager_ext/value.h>
#include <lager/lenses.hpp>

namespace lager_ext {

// ============================================================
// Demo function for lager::lenses::at with Value
//
// Note: No additional functions are needed here!
// Value already implements the container interface (at, set, count, size)
// so lager::lenses::at works out of the box.
// ============================================================
void demo_at_lens();

} // namespace lager_ext

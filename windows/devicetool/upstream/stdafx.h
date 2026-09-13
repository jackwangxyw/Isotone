/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2017  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

// Isotone modification: upstream includes helpers/ScopeGuard.h, which pulls in
// folly's UncaughtExceptions.h (Apache-2.0, no GPL header). The vendored files
// only use SCOPE_EXIT, so a minimal equivalent is defined here instead.

#define _USE_MATH_DEFINES
#include <cmath>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <utility>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Shlwapi.h>
#include <Ks.h>
#include <KsMedia.h>

namespace isotone_scope_exit {
template <typename F> class Guard
{
public:
	explicit Guard(F&& f) : f(std::move(f)) {}
	~Guard() { f(); }
private:
	F f;
};
enum class OnExit {};
template <typename F> Guard<F> operator+(OnExit, F&& f) { return Guard<F>(std::forward<F>(f)); }
}

#define ISOTONE_SCOPE_CONCAT_IMPL(a, b) a##b
#define ISOTONE_SCOPE_CONCAT(a, b) ISOTONE_SCOPE_CONCAT_IMPL(a, b)
#define SCOPE_EXIT auto ISOTONE_SCOPE_CONCAT(scope_exit_, __COUNTER__) = ::isotone_scope_exit::OnExit() + [&]()

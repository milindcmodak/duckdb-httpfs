#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct THFSTokenResolver {
	//! Resolve thfs bearer token from (in order): setting, env var, optional secret fallback.
	static string ResolveToken(optional_ptr<FileOpener> opener, const string &path);
};

} // namespace duckdb

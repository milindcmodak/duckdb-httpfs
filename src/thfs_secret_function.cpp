#include "thfs_secret_function.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <cstdlib>

namespace duckdb {

string THFSTokenResolver::ResolveToken(optional_ptr<FileOpener> opener, const string &path) {
	if (opener) {
		Value setting_val;
		if (FileOpener::TryGetCurrentSetting(opener, "thfs_token", setting_val)) {
			auto token = setting_val.ToString();
			if (!token.empty()) {
				return token;
			}
		}
	}

	auto env_token = std::getenv("THFS_BEARER_TOKEN");
	if (env_token && env_token[0] != '\0') {
		return string(env_token);
	}

	// Optional backward-compatible secret fallback
	auto secret_manager = FileOpener::TryGetSecretManager(opener);
	auto transaction = FileOpener::TryGetCatalogTransaction(opener);
	if (!secret_manager || !transaction) {
		return "";
	}

	auto bearer_match = secret_manager->LookupSecret(*transaction, path, "bearer");
	if (bearer_match.HasMatch()) {
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*bearer_match.secret_entry->secret);
		return kv_secret.TryGetValue("token", true).ToString();
	}

	auto turbo_match = secret_manager->LookupSecret(*transaction, path, "turbohttpfs");
	if (turbo_match.HasMatch()) {
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*turbo_match.secret_entry->secret);
		return kv_secret.TryGetValue("token", true).ToString();
	}

	return "";
}

} // namespace duckdb

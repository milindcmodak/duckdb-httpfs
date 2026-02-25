#include "thfs.hpp"

#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "http_state.hpp"
#include "thfs_secret_function.hpp"

namespace duckdb {

TurboHTTPFileSystem::~TurboHTTPFileSystem() {
}

THFSFileHandle::~THFSFileHandle() {
}

unique_ptr<HTTPClient> THFSFileHandle::CreateClient() {
	return http_params.http_util.InitializeClient(http_params, parsed_url.endpoint);
}

static bool Match(vector<string>::const_iterator key, vector<string>::const_iterator key_end,
                  vector<string>::const_iterator pattern, vector<string>::const_iterator pattern_end) {
	while (key != key_end && pattern != pattern_end) {
		if (*pattern == "**") {
			if (std::next(pattern) == pattern_end) {
				return true;
			}
			while (key != key_end) {
				if (Match(key, key_end, std::next(pattern), pattern_end)) {
					return true;
				}
				key++;
			}
			return false;
		}
		if (!Glob(key->data(), key->length(), pattern->data(), pattern->length())) {
			return false;
		}
		key++;
		pattern++;
	}
	return key == key_end && pattern == pattern_end;
}

static string NormalizeDir(const string &path) {
	if (path.empty() || path == "/") {
		return "";
	}
	if (path.back() == '/') {
		return path.substr(0, path.size() - 1);
	}
	return path;
}

static string JoinPathParts(const string &lhs, const string &rhs) {
	auto left = NormalizeDir(lhs);
	if (left.empty()) {
		return "/" + rhs;
	}
	return left + "/" + rhs;
}

static bool ParseBooleanValueAt(const string &input, idx_t idx, bool &result) {
	if (idx + 4 <= input.size() && strncmp(input.c_str() + idx, "true", 4) == 0) {
		result = true;
		return true;
	}
	if (idx + 5 <= input.size() && strncmp(input.c_str() + idx, "false", 5) == 0) {
		result = false;
		return true;
	}
	return false;
}

static void ParseListResult(const string &input, const string &base_dir, vector<string> &files, vector<string> &dirs) {
	idx_t idx = 0;
	while (idx < input.size()) {
		if (input[idx] != '{') {
			idx++;
			continue;
		}
		idx++;
		string name;
		optional<bool> is_directory;
		while (idx < input.size() && input[idx] != '}') {
			if (strncmp(input.c_str() + idx, "\"name\":\"", 8) == 0) {
				idx += 8;
				while (idx < input.size()) {
					if (input[idx] == '\\' && idx + 1 < input.size()) {
						idx++;
						name.push_back(input[idx]);
						idx++;
						continue;
					}
					if (input[idx] == '"') {
						idx++;
						break;
					}
					name.push_back(input[idx]);
					idx++;
				}
				continue;
			}
			if (strncmp(input.c_str() + idx, "\"directory\":", 12) == 0) {
				idx += 12;
				bool boolean_value;
				if (!ParseBooleanValueAt(input, idx, boolean_value)) {
					throw IOException("Failed to parse thfs index result");
				}
				is_directory = boolean_value;
				idx += boolean_value ? 4 : 5;
				continue;
			}
			idx++;
		}
		if (idx < input.size() && input[idx] == '}') {
			idx++;
		}
		if (name.empty() || !is_directory.IsValid()) {
			continue;
		}
		auto joined_path = JoinPathParts(base_dir, name);
		if (is_directory.GetValue()) {
			dirs.push_back(joined_path);
		} else {
			files.push_back(joined_path);
		}
	}
}

string TurboHTTPFileSystem::ListRequest(const ParsedTHFSUrl &url, HTTPFSParams &http_params, optional_ptr<HTTPState> state) {
	(void)state;
	HTTPHeaders header_map;
	std::stringstream response;
	auto list_url = GetListUrl(url);
	GetRequestInfo get_request(
	    url.endpoint, list_url, header_map, http_params,
	    [&](const HTTPResponse &response_meta) {
		    if (static_cast<int>(response_meta.status) >= 400) {
			    throw HTTPException(response_meta, "HTTP GET error on '%s' (HTTP %d)", list_url, response_meta.status);
		    }
		    return true;
	    },
	    [&](const_data_ptr_t data, idx_t data_length) {
		    response << string(const_char_ptr_cast(data), data_length);
		    return true;
	    });
	auto res = http_params.http_util.Request(get_request);
	if (res->status != HTTPStatusCode::OK_200) {
		throw IOException(res->GetError() + " error for HTTP GET to '" + list_url + "'");
	}
	return response.str();
}

vector<OpenFileInfo> TurboHTTPFileSystem::Glob(const string &path, FileOpener *opener) {
	auto http_use_ssl = false;
	if (opener) {
		Value setting_val;
		if (FileOpener::TryGetCurrentSetting(opener, "thfs_use_ssl", setting_val)) {
			http_use_ssl = setting_val.GetValue<bool>();
		}
	}
	auto parsed_glob_url = THFSUrlParse(path, http_use_ssl);
	auto first_wildcard_pos = parsed_glob_url.path.find_first_of("*[\\");
	if (first_wildcard_pos == string::npos) {
		return {path};
	}

	string shared_path = parsed_glob_url.path.substr(0, first_wildcard_pos);
	auto last_path_slash = shared_path.find_last_of('/', first_wildcard_pos);
	if (last_path_slash == string::npos) {
		shared_path = "";
	} else {
		shared_path = shared_path.substr(0, last_path_slash);
	}

	FileOpenerInfo info;
	info.file_path = path;
	auto http_util = HTTPFSUtil::GetHTTPUtil(opener);
	auto params = http_util->InitializeParameters(opener, info);
	auto &http_params = params->Cast<HTTPFSParams>();
	SetParams(http_params, path, opener);
	auto http_state = HTTPState::TryGetState(opener).get();

	ParsedTHFSUrl current = parsed_glob_url;
	current.path = shared_path;

	vector<string> files;
	vector<string> dirs = {shared_path};

	while (!dirs.empty()) {
		current.path = dirs.back();
		dirs.pop_back();
		auto response_str = ListRequest(current, http_params, http_state);
		ParseListResult(response_str, current.path, files, dirs);
	}

	vector<string> pattern_splits = StringUtil::Split(parsed_glob_url.path, "/");
	vector<OpenFileInfo> result;
	for (const auto &file : files) {
		vector<string> file_splits = StringUtil::Split(file, "/");
		bool is_match = Match(file_splits.begin(), file_splits.end(), pattern_splits.begin(), pattern_splits.end());
		if (is_match) {
			auto matched = parsed_glob_url;
			matched.path = file;
			result.push_back(GetTHFSUrl(matched));
		}
	}
	return result;
}

unique_ptr<HTTPResponse> TurboHTTPFileSystem::HeadRequest(FileHandle &handle, string thfs_url, HTTPHeaders header_map) {
	(void)thfs_url;
	auto &thfs_handle = handle.Cast<THFSFileHandle>();
	auto http_url = GetFileUrl(thfs_handle.parsed_url);
	return HTTPFileSystem::HeadRequest(handle, http_url, header_map);
}

unique_ptr<HTTPResponse> TurboHTTPFileSystem::GetRequest(FileHandle &handle, string thfs_url, HTTPHeaders header_map) {
	(void)thfs_url;
	auto &thfs_handle = handle.Cast<THFSFileHandle>();
	auto http_url = GetFileUrl(thfs_handle.parsed_url);
	return HTTPFileSystem::GetRequest(handle, http_url, header_map);
}

unique_ptr<HTTPResponse> TurboHTTPFileSystem::GetRangeRequest(FileHandle &handle, string thfs_url, HTTPHeaders header_map,
                                                              idx_t file_offset, char *buffer_out,
                                                              idx_t buffer_out_len) {
	(void)thfs_url;
	auto &thfs_handle = handle.Cast<THFSFileHandle>();
	auto http_url = GetFileUrl(thfs_handle.parsed_url);
	return HTTPFileSystem::GetRangeRequest(handle, http_url, header_map, file_offset, buffer_out, buffer_out_len);
}

unique_ptr<HTTPFileHandle> TurboHTTPFileSystem::CreateHandle(const OpenFileInfo &file, FileOpenFlags flags,
                                                             optional_ptr<FileOpener> opener) {
	D_ASSERT(flags.Compression() == FileCompressionType::UNCOMPRESSED);

	auto http_use_ssl = false;
	if (opener) {
		Value setting_val;
		if (FileOpener::TryGetCurrentSetting(opener, "thfs_use_ssl", setting_val)) {
			http_use_ssl = setting_val.GetValue<bool>();
		}
	}
	auto parsed_url = THFSUrlParse(file.path, http_use_ssl);

	FileOpenerInfo info;
	info.file_path = file.path;
	auto http_util = HTTPFSUtil::GetHTTPUtil(opener);
	auto params = http_util->InitializeParameters(opener, info);
	SetParams(params->Cast<HTTPFSParams>(), file.path, opener);

	return duckdb::make_uniq<THFSFileHandle>(*this, std::move(parsed_url), file, flags, std::move(params));
}

void TurboHTTPFileSystem::SetParams(HTTPFSParams &params, const string &path, optional_ptr<FileOpener> opener) {
	params.bearer_token = THFSTokenResolver::ResolveToken(opener, path);
}

static void ThrowParseError(const string &url) {
	throw IOException("Failed to parse '%s'. Please format url like: 'thfs://host[:port]/path/to/file.parquet'", url);
}

ParsedTHFSUrl TurboHTTPFileSystem::THFSUrlParse(const string &url, bool use_ssl) {
	if (!StringUtil::StartsWith(url, "thfs://")) {
		throw InternalException("Not a thfs url");
	}
	auto offset = string("thfs://").size();
	auto slash = url.find('/', offset);
	if (slash == string::npos) {
		ThrowParseError(url);
	}
	auto authority = url.substr(offset, slash - offset);
	if (authority.empty()) {
		ThrowParseError(url);
	}
	ParsedTHFSUrl result;
	result.endpoint = string(use_ssl ? "https://" : "http://") + authority;
	result.path = url.substr(slash);
	if (result.path.empty()) {
		result.path = "/";
	}
	return result;
}

string TurboHTTPFileSystem::GetTHFSUrl(const ParsedTHFSUrl &url) {
	if (!StringUtil::StartsWith(url.endpoint, "http://") && !StringUtil::StartsWith(url.endpoint, "https://")) {
		throw IOException("Invalid thfs endpoint '%s'", url.endpoint);
	}
	auto offset = url.endpoint.find("://");
	D_ASSERT(offset != string::npos);
	return "thfs://" + url.endpoint.substr(offset + 3) + url.path;
}

string TurboHTTPFileSystem::GetListUrl(const ParsedTHFSUrl &url) {
	auto http_url = url.endpoint + NormalizeDir(url.path);
	if (!StringUtil::EndsWith(http_url, "/")) {
		http_url += "/";
	}
	http_url += "index.json";
	return http_url;
}

string TurboHTTPFileSystem::GetFileUrl(const ParsedTHFSUrl &url) {
	return url.endpoint + url.path;
}

} // namespace duckdb

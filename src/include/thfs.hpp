#pragma once

#include "httpfs.hpp"

namespace duckdb {

struct ParsedTHFSUrl {
	string endpoint;
	string path;
};

class TurboHTTPFileSystem : public HTTPFileSystem {
public:
	~TurboHTTPFileSystem() override;

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	duckdb::unique_ptr<HTTPResponse> HeadRequest(FileHandle &handle, string thfs_url, HTTPHeaders header_map) override;
	duckdb::unique_ptr<HTTPResponse> GetRequest(FileHandle &handle, string thfs_url, HTTPHeaders header_map) override;
	duckdb::unique_ptr<HTTPResponse> GetRangeRequest(FileHandle &handle, string thfs_url, HTTPHeaders header_map,
	                                                 idx_t file_offset, char *buffer_out,
	                                                 idx_t buffer_out_len) override;

	bool CanHandleFile(const string &fpath) override {
		return fpath.rfind("thfs://", 0) == 0;
	}

	string GetName() const override {
		return "TurboHTTPFileSystem";
	}

	static ParsedTHFSUrl THFSUrlParse(const string &url, bool use_ssl = true);
	static string GetTHFSUrl(const ParsedTHFSUrl &url);
	static string GetListUrl(const ParsedTHFSUrl &url);
	static string GetFileUrl(const ParsedTHFSUrl &url);

	static void SetParams(HTTPFSParams &params, const string &path, optional_ptr<FileOpener> opener);

protected:
	duckdb::unique_ptr<HTTPFileHandle> CreateHandle(const OpenFileInfo &file, FileOpenFlags flags,
	                                                optional_ptr<FileOpener> opener) override;

	string ListRequest(const ParsedTHFSUrl &url, HTTPFSParams &http_params, optional_ptr<HTTPState> state);
};

class THFSFileHandle : public HTTPFileHandle {
	friend class TurboHTTPFileSystem;

public:
	THFSFileHandle(FileSystem &fs, ParsedTHFSUrl thfs_url, const OpenFileInfo &file, FileOpenFlags flags,
	               unique_ptr<HTTPParams> http_params)
	    : HTTPFileHandle(fs, file, flags, std::move(http_params)), parsed_url(std::move(thfs_url)) {
	}
	~THFSFileHandle() override;

	unique_ptr<HTTPClient> CreateClient() override;

protected:
	ParsedTHFSUrl parsed_url;
};

} // namespace duckdb

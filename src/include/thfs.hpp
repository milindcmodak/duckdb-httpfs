#pragma once

#include "httpfs.hpp"

namespace duckdb {

struct ParsedTHFSUrl {
	string endpoint;
	string path;
};

class TurboHTTPTransport;

class TurboHTTPFileSystem : public FileSystem {
public:
	TurboHTTPFileSystem();
	~TurboHTTPFileSystem() override;

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;

	bool CanHandleFile(const string &fpath) override;
	string GetName() const override;
	string PathSeparator(const string &path) override;
	bool CanSeek() override;
	bool OnDiskFile(FileHandle &handle) override;
	bool IsPipe(const string &filename, optional_ptr<FileOpener> opener) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void FileSync(FileHandle &handle) override;
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;

	static ParsedTHFSUrl THFSUrlParse(const string &url, bool use_ssl = true);
	static string GetTHFSUrl(const ParsedTHFSUrl &url);
	static string GetListUrl(const ParsedTHFSUrl &url);
	static string GetFileUrl(const ParsedTHFSUrl &url);
	static void SetParams(HTTPFSParams &params, const string &path, optional_ptr<FileOpener> opener);

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;
	bool SupportsOpenFileExtended() const override;

private:
	unique_ptr<TurboHTTPTransport> transport;
};

class THFSFileHandle : public HTTPFileHandle {
	friend class TurboHTTPTransport;

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

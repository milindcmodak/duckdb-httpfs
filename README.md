# DuckDB THFSS extension

The thfss extension is a standalone extension implementing remote filesystems (HTTP, S3 API, Hugging Face, and TurboHTTPFileSystem). For plain HTTP(S), only file reading is supported. For object storage using the S3 API, the extension supports reading/writing/globbing files.

## Building & Loading the Extension

The DuckDB submodule must be initialized prior to building.
```bash
git submodule init
git pull --recurse-submodules
```

To build, type:
```
make setup-vcpkg
VCPKG_TOOLCHAIN_PATH=$pwd/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja make
```
Consider adding `GEN=ninja` and having `ccache` installed to speed up recompilations.

### VCPKG
`vcpkg`, a package manager for C++, it's highly reccomended to generate reproducible and stable builds, in particular here it serves to build the `openssl` and `curl` dependencies.
Without the `VCPKG_TOOLCHAIN_PATH` option, locally available libraries will be used from default search paths.

## Running
The resulting binary, that will also statically link and load the `httfps`, it's available like:
```
./build/release/duckdb
```
```sql
FROM read_blob('https://duckdb.org/');
```

## Testing
Some tests querying remote resources can be run already without further setup:
```
./build/release/test/unittest
```
Further integration testing uses a local MinIO setup using Docker. See the [testing documentation for more information on how to set this up locally](test).


## TurboHTTPFileSystem (`thfs://`)

This standalone `thfss` extension includes a dedicated `TurboHTTPFileSystem` subsystem that is separate from `HTTPFileSystem` and `HuggingFaceFileSystem`.

### URL scheme
- `thfs://host[:port]/path/to/file.parquet`
- Example:
  ```sql
  SELECT *
  FROM read_parquet('thfs://127.0.0.1:10009/tmp_123e4567-e89b-12d3-a456-426614174000/data/*.parquet');
  ```

### Authentication (Bearer token)
`thfs://` can authenticate without secrets. Token resolution order is:
1. DuckDB setting `thfs_token`
2. Environment variable `THFS_BEARER_TOKEN`
3. (optional fallback) DuckDB secrets (`bearer` or `turbohttpfs`)

```sql
SET thfs_token='my-secret-token';
```

### GLOB support
`thfs://` supports wildcard expansion (`*`, `?`, `[]`, `**`) by recursively reading `index.json` directory listings from the backend.

### HTTP/HTTPS backend selection
By default, `thfs://` maps to HTTP for network calls. For HTTPS backends, enable SSL explicitly:

```sql
SET thfs_use_ssl = true;
```

## Building into a custom DuckDB binary
This repository builds a standalone `thfss` extension for DuckDB custom builds. `TurboHTTPFileSystem` is compiled into the `thfss` binary, so it does not modify or depend on the standard `httpfs` extension binary.


## Integrating into a custom DuckDB build (as extension)
Use DuckDB's extension loading flow and include this repository as an external extension entry (`thfss`) in your DuckDB build configuration. This produces `thfss` as a separate binary/target from standard `httpfs`.

## Integrating directly into DuckDB core (not as extension)
Yes, this can be compiled directly into DuckDB instead of being a loadable extension.

- `TurboHTTPFileSystem` is standalone (does not derive from `HTTPFileSystem`) and delegates transport I/O to an internal HTTP transport object.
- Connection reuse/caching is enabled via `HTTPFileHandle` client cache used by `THFSFileHandle`.

Recommended file set for core integration:
- `thfs.hpp`
- `thfs.cpp`
- `thfs_secret_function.hpp`
- `thfs_secret_function.cpp`

High-level steps:
1. Copy those files into DuckDB source tree (e.g. alongside other filesystem implementations).
2. Add them to DuckDB core CMake/source lists.
3. Register `TurboHTTPFileSystem` during DB initialization where filesystems are registered.
4. Register configuration options:
   - `thfs_use_ssl` (default `false` for HTTP)
   - `thfs_token` (default empty, reads `THFS_BEARER_TOKEN` when empty)
5. If desired, skip secret providers entirely; auth works with setting/env variable only.

This gives you a true built-in `thfs://` filesystem in DuckDB without requiring `LOAD thfss`.

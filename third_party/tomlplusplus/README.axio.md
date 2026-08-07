# toml++ vendoring record

- Upstream: https://github.com/marzer/tomlplusplus
- Version: 3.4.0
- Archive: https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
- Archive SHA-256: `8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155`
- Imported files: upstream `include/` tree and `LICENSE`

Axio vendors this header-only dependency so remote and offline builds use the
same TOML parser without fetching network content during Meson configuration.

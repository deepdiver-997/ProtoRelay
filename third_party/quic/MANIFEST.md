# QUIC third party（vendored，不入库）

本目录承载 QUIC 传输后端（`protorelay_quic` 模块）的第三方依赖。**源码与构建
产物只存在于各检出（worktree）里，不提交**——入库的只有本清单和补丁。

## 版本钉

| 仓库 | 钉定 commit | 说明 |
|---|---|---|
| lsquic | `f9061608d6a39ef04cd5253d60bcb9da1326345f` | "Make the CRYPTO-frame stash limit configurable (default 20)"，v4.9.4 之后 |
| BoringSSL | `956bac6e4db9b33789dcedb5ea3f28e51030cead` | "add CBS_get_u48" |

lsquic 在基线上带两个本地补丁（见 `patches/`，决策依据见
`docs/architecture/quic-backend.md`）：

- **Patch A**（无 SNI 回退默认证书）：只影响 `LSENG_HTTP` 模式；本模块走裸
  QUIC 流，属惰性代码。保留以与既有部署同源。
- **Patch B**（RFC 9000 §14.1 Initial 填包重启用）：上游真实违规，严格对端
  （ngtcp2 系）会丢弃未填包的 Initial 导致握手挂死。**必须保留**。

## 符号前缀

本仓库进程内已有系统 OpenSSL（Asio/native TLS），BoringSSL 必须以
`BORINGSSL_PREFIX=pr_bssl_` 全量符号重命名构建（gRPC 同款）。该 BoringSSL
版本把前缀无关的 `include/openssl/prefix_symbols.h` track 在上游树里，
编译期由 `BORINGSSL_PREFIX` 宏展开——不需要 Go，也不需要额外生成步骤。

## 获取与构建（在本目录下手动执行，一次）

```bash
# 1) 拉取并钉定，打补丁（网络受限时先给 git 配好代理）
git clone --no-checkout https://github.com/litespeedtech/lsquic lsquic
git -C lsquic checkout f9061608d6a39ef04cd5253d60bcb9da1326345f
git -C lsquic apply patches/lsquic-http3-interop-a-b.patch
git clone --no-checkout https://github.com/google/boringssl boringssl
git -C boringssl checkout 956bac6e4db9b33789dcedb5ea3f28e51030cead

# 2) 构建 prefixed BoringSSL（静态库 ssl + crypto）
cmake -S boringssl -B boringssl/build -DCMAKE_BUILD_TYPE=Release \
    -DBORINGSSL_PREFIX=pr_bssl_ -DBUILD_TESTING=OFF
cmake --build boringssl/build -j4 --target ssl crypto

# 3) 构建 lsquic（静态库），指向第 2 步的产物
cmake -S lsquic -B lsquic/build -DCMAKE_BUILD_TYPE=Release \
    -DLSQUIC_LIBSSL=BORINGSSL \
    -DBORINGSSL_INCLUDE="$PWD/boringssl/include" \
    -DBORINGSSL_LIB_ssl="$PWD/boringssl/build/libssl.a" \
    -DBORINGSSL_LIB_crypto="$PWD/boringssl/build/libcrypto.a" \
    -DLSQUIC_SHARED_LIB=OFF -DLSQUIC_BIN=OFF -DLSQUIC_TESTS=OFF \
    -DLSQUIC_DEVEL=OFF -DCMAKE_C_FLAGS="-DBORINGSSL_PREFIX=pr_bssl_"
cmake --build lsquic/build -j4 --target lsquic

# 4) 自检：libssl.a 必须只剩带前缀的符号（macOS nm 的 C 符号多一个前导下划线）
if nm -g boringssl/build/libssl.a | grep -E "[ T]_?SSL_new$"; then
    echo "FATAL: prefix build failed"; exit 1
fi
```

构建完成后，主 CMake 自动探测 `lsquic/build/src/liblsquic/liblsquic.a` 与
`boringssl/build/lib{ssl,crypto}.a` 并启用 `protorelay_quic` 模块；第三方缺席时
整个模块（含其测试）按不存在处理，contracts-only / framework-only 不受影响。

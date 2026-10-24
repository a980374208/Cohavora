# Cohavora 升级发行工具

客户端优先读取阿里云 ECS 静态 Web 清单 `http://123.56.225.164/downloads/latest.json`，失败时读取 GitHub Releases 备用清单。安装包也按签名清单中的地址顺序尝试。此目录只生成、验证本地发行文件，不操作服务器、不上传文件、不发布 GitHub Release。

当前软件版本为 `0.0.1`。客户端仅在清单版本高于当前版本时提供升级，不降级；同为 `0.0.1` 时不会提示更新。以下 `0.0.2` 仅用于演示下一版本的发行命令；下一次发行应先同步产品版本，再填写新版本的发行配置。

## 发行配置

复制 [release-config.example.json](release-config.example.json) 到本机发行目录，例如 `out/installer/release-config.json`，更新以下字段：

```json
{
  "latest_version": "0.0.2",
  "force_update": false,
  "download_url": "http://123.56.225.164/downloads/Cohavora-0.0.2-x64-Setup.exe",
  "fallback_download_url": "https://github.com/a980374208/Cohavora/releases/download/v0.0.2/Cohavora-0.0.2-x64-Setup.exe",
  "release_notes": "本次更新说明"
}
```

| 字段 | 约束 |
| --- | --- |
| `latest_version` | 必填。无前导零的 `X.Y.Z`，每段不超过 `2147483647` |
| `force_update` | 必填。严格 JSON 布尔值；不能使用 `"true"`、`1` 或 `null` |
| `download_url` | 必填。最新版本安装包直连地址 |
| `fallback_download_url` | 可选。必须使用 HTTPS；缺省为当前版本对应的 Cohavora GitHub Release 安装包地址 |
| `release_notes` | 可选。纯文本，缺省为空，最多 8 KiB UTF-8 |
| `published_at` | 可选。UTC `YYYY-MM-DDTHH:MM:SSZ`；缺省为生成清单时的当前 UTC |

未知字段、重复 JSON 字段和无效类型会被拒绝。`force_update` 会写入并验证签名清单，**本阶段暂缓客户端强制更新控制，不阻断登录或入会**，仍由用户手动检查、下载和启动安装器。

下载 URL 不允许凭据、fragment、反斜杠、空白或控制字符。HTTPS 使用默认端口或 443；HTTP 只允许 `123.56.225.164` 的默认端口或显式 `:80`，对象必须位于 `/downloads/` 下。HTTP 路径各段只能包含 ASCII 字母、数字、点、下划线和连字符，不能有 `.`、`..`、空段、末尾斜线、百分号编码或 query。普通 HTTP 站点、其他端口和其他路径均被拒绝。客户端禁止 HTTPS 重定向降级至 HTTP；HTTP 主站重定向仍必须符合这一地址策略。

## 依赖与密钥

工具使用 Python 3.9+ 标准库及支持 Ed25519 的 OpenSSL 1.1.1+ CLI，无 Python 第三方依赖。`--openssl` 必须放在子命令之前；OpenSSL 已加入 PATH 时可以省略。

在发行机器上选择仓库外、仅发行者可访问的密钥目录。`keygen` 拒绝在 Git 工作树内生成私钥，也拒绝覆盖现有私钥；Windows 目录应预先设置访问权限。私钥只用于离线签名，**不进入源码、客户端、Web 目录或 GitHub Release**。

```powershell
$OpenSsl = 'C:\Program Files\OpenSSL-Win64\bin\openssl.exe' # 改成实际路径
$PrivateKey = Read-Host '输入仓库外受保护目录中的新私钥文件完整路径'
$PublicKeyHex = python packaging/updates/update_manifest.py --openssl $OpenSsl keygen --private-key $PrivateKey
if ($LASTEXITCODE -ne 0) { throw '生成密钥失败' }
$PublicKeyHex
```

输出的 64 个十六进制字符代表 32 字节 Ed25519 原始公钥，不是 PEM 文本或 DER 文件的十六进制。已有私钥可用 `public-key --private-key $PrivateKey` 导出公钥。发行密钥应跨版本保持一致；更换密钥需先通过可信软件版本交付新公钥。

正式发行公钥已配置在 `cmake/CohavoraUpdates.cmake`，值为 `20cdd8328dd6f7ddc2842be3897176b2803e9b3b7ebd71da6c8ce4fbaeb13e42`。后续发行复用对应私钥，不重新执行 `keygen`。已有构建缓存若保留了空公钥，须在本机预设或 configure 参数中显式设置此公钥，再重新构建及打包；修改配置不会改变已生成的安装器。私钥保留在仓库外受限目录，并由发行者另行安全备份。

HTTP 无法保护传输，客户端依靠固定可信公钥验证清单，再检查安装器大小和 SHA-256。它可拒绝篡改文件，但不能防止阻断或旧签名清单重放。没有配置可信公钥时，客户端拒绝升级请求。

## 客户端配置

将以下 CMake cache 参数加入当前工具链的产品 configure 命令或本机预设。主源和备用源均可配置，当前主源为阿里云 ECS Web 地址：

```powershell
# 添加到现有产品 configure 命令：
# -DCOHAVORA_UPDATE_PRIMARY_URL="http://123.56.225.164/downloads/latest.json"
# -DCOHAVORA_UPDATE_PUBLIC_KEY_HEX="$PublicKeyHex"
# 可选覆盖备用地址，默认如下：
# -DCOHAVORA_UPDATE_FALLBACK_URL="https://github.com/a980374208/Cohavora/releases/latest/download/latest.json"
```

公钥可公开；私钥和服务器上传凭据只保留在受保护的发行环境中。安装器应在配置正确的产品构建完成后，通过现有 [Inno Setup 脚本](../windows/CohavoraInstaller.iss) 生成。客户端不需要阿里云 AccessKey。

## 生成和验证清单

从仓库根目录执行。安装器必须已经完成打包；签名后更改安装器字节会使验证失败。安装器大小上限为 2 GiB。

```powershell
$Installer = 'out/installer/Cohavora-0.0.2-x64-Setup.exe'
$Config = 'out/installer/release-config.json'
$Manifest = 'out/installer/latest.json'
python packaging/updates/update_manifest.py --openssl $OpenSsl create `
    --private-key $PrivateKey --installer $Installer --config $Config --output $Manifest
if ($LASTEXITCODE -ne 0) { throw '创建清单失败' }
python packaging/updates/update_manifest.py --openssl $OpenSsl verify `
    --manifest $Manifest --public-key-hex $PublicKeyHex --installer $Installer
if ($LASTEXITCODE -ne 0) { throw '发行验证失败' }
```

工具从配置读取版本、更新标记和下载地址，计算安装包大小及 SHA-256，生成签名 `latest.json`。发行配置用于离线生成，客户端实际读取的是签名清单。**不能直接手工修改已签名的 `latest.json`**；任何配置或安装包改动都需要重新 `create` 并 `verify`。输出文件原子替换，不覆盖输入配置、说明文件、私钥或安装器。

原有 CLI 仍可使用，`--url` 可重复，依次指定阿里云和 GitHub 下载地址；新增 `--force-update` 记录 `true`，缺省为 `false`：

```powershell
python packaging/updates/update_manifest.py --openssl $OpenSsl create `
    --private-key $PrivateKey --installer $Installer --version '0.0.2' `
    --url 'http://123.56.225.164/downloads/Cohavora-0.0.2-x64-Setup.exe' `
    --url 'https://github.com/a980374208/Cohavora/releases/download/v0.0.2/Cohavora-0.0.2-x64-Setup.exe' `
    --notes '本次更新的纯文本说明' --output $Manifest
```

CLI 模式还支持 `--notes-file <UTF-8纯文本路径>` 和 `--published-at <UTC时间>`。`--config` 不能与 `--version`、`--url`、`--force-update`、`--notes`、`--notes-file` 或 `--published-at` 混用，避免配置覆盖歧义。

## 清单协议

外层 `latest.json` 是 UTF-8 JSON，恰好包含两个标准 base64 字段：

```json
{"payload":"base64(UTF-8 payload bytes)","signature":"base64(64-byte Ed25519 signature)"}
```

签名对象是 payload 的原始 UTF-8 字节，不是 base64 文本、哈希或重新序列化后的 JSON。工具使用确定的紧凑 JSON 序列化后签名；验签必须先于使用版本、说明和下载地址。

解码后的 payload 如下；`sha256` 和 `size` 仅为结构示例，实际值由工具从安装器计算。字段 `force_update` 已包含在签名内，旧清单缺失该字段时按 `false` 兼容读取。

```json
{
  "schema": 1,
  "product": "Cohavora",
  "channel": "stable",
  "version": "0.0.2",
  "force_update": false,
  "published_at": "2026-10-12T00:00:00Z",
  "notes": "本次更新说明",
  "windows-x64": {
    "size": 123,
    "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
    "urls": [
      "http://123.56.225.164/downloads/Cohavora-0.0.2-x64-Setup.exe",
      "https://github.com/a980374208/Cohavora/releases/download/v0.0.2/Cohavora-0.0.2-x64-Setup.exe"
    ]
  }
}
```

清单最大 96 KiB，payload 最大 48 KiB，说明最大 8 KiB UTF-8，下载地址数量为 1–8。主备清单使用同一 Ed25519 签名密钥；安装包的版本、大小与 SHA-256 必须匹配已接受清单。服务端无需解析或重签清单，只提供原始字节。

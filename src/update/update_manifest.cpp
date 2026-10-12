#include "update_manifest.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <openssl/evp.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace Cohavora::Update {
namespace {

constexpr int MaxPayloadBytes = 48 * 1024;
constexpr int MaxNotesBytes = 8 * 1024;
constexpr int MaxUrlBytes = 4096;

bool IsText(const QString &text, bool url = false) {
    for (int at = 0; at < text.size(); ++at) {
        const auto character = text[at];
        if (character.isHighSurrogate()) {
            if (++at == text.size() || !text[at].isLowSurrogate()) return false;
        } else if (character.isLowSurrogate()) {
            return false;
        } else {
            const auto code = character.unicode();
            if (code == 127) return false;
            if (url) {
                if (code <= 32 || code == '\\' || code == '#') return false;
            } else if (code < 32 && code != '\r' && code != '\n' && code != '\t') {
                return false;
            }
        }
    }
    return !url || text.toUtf8().size() <= MaxUrlBytes;
}

std::optional<std::array<int, 3>> ParseVersion(const QString &version) {
    std::array<int, 3> parts{};
    int offset = 0;
    for (int part = 0; part != 3; ++part) {
        const int start = offset;
        int value = 0;
        while (offset < version.size() && version[offset] != QLatin1Char('.')) {
            const auto character = version[offset].unicode();
            if (character < '0' || character > '9') return std::nullopt;
            const int digit = character - '0';
            if (value > (std::numeric_limits<int>::max() - digit) / 10) {
                return std::nullopt;
            }
            value = value * 10 + digit;
            ++offset;
        }
        if (offset == start
            || (offset - start > 1 && version[start] == QLatin1Char('0'))) {
            return std::nullopt;
        }
        parts[part] = value;
        if (part < 2) {
            if (offset >= version.size()) return std::nullopt;
            ++offset;
        } else if (offset != version.size()) {
            return std::nullopt;
        }
    }
    return parts;
}

// Qt's DOM parser keeps the last duplicate object member. Walk its already
// validated input as well, comparing decoded keys so "version" and an escaped
// spelling cannot give a signed document two different interpretations.
class UniqueJsonKeys final {
public:
    explicit UniqueJsonKeys(const QByteArray &input, bool payload)
        : input_(input), payload_(payload) {}

    bool Check() {
        if (!ReadValue(0, ObjectContext::Root)) return false;
        SkipSpace();
        return offset_ == input_.size();
    }

private:
    enum class ObjectContext { Other, Root, Installer };

    char Current() const {
        return offset_ < input_.size() ? input_[offset_] : '\0';
    }

    void SkipSpace() {
        while (Current() == ' ' || Current() == '\r'
               || Current() == '\n' || Current() == '\t') {
            ++offset_;
        }
    }

    bool Take(char value) {
        SkipSpace();
        if (Current() != value) return false;
        ++offset_;
        return true;
    }

    bool ReadString(QString *decoded = nullptr) {
        if (Current() != '"') return false;
        const int start = offset_++;
        while (offset_ < input_.size()) {
            const char character = input_[offset_++];
            if (character == '\\') {
                if (offset_ == input_.size()) return false;
                if (Current() == 'u') {
                    const auto code = ReadHex16(offset_ + 1);
                    if (!code) return false;
                    offset_ += 5;
                    if (*code >= 0xd800 && *code <= 0xdbff) {
                        if (input_.mid(offset_, 2) != "\\u") return false;
                        const auto low = ReadHex16(offset_ + 2);
                        if (!low || *low < 0xdc00 || *low > 0xdfff) return false;
                        offset_ += 6;
                    } else if (*code >= 0xdc00 && *code <= 0xdfff) {
                        return false;
                    }
                } else {
                    ++offset_;
                }
            } else if (character == '"') {
                if (decoded) {
                    const auto token = input_.mid(start, offset_ - start);
                    const auto document = QJsonDocument::fromJson('[' + token + ']');
                    if (!document.isArray() || document.array().size() != 1) return false;
                    *decoded = document.array().at(0).toString();
                }
                return true;
            }
        }
        return false;
    }

    std::optional<unsigned int> ReadHex16(int at) const {
        if (at + 4 > input_.size()) return std::nullopt;
        unsigned int value = 0;
        for (int end = at + 4; at < end; ++at) {
            const char character = input_[at];
            if (character >= '0' && character <= '9') value = value * 16 + character - '0';
            else if (character >= 'a' && character <= 'f') value = value * 16 + character - 'a' + 10;
            else if (character >= 'A' && character <= 'F') value = value * 16 + character - 'A' + 10;
            else return std::nullopt;
        }
        return value;
    }

    bool ReadValue(int depth, ObjectContext context = ObjectContext::Other,
                   bool integerToken = false) {
        if (depth > 32) return false;
        SkipSpace();
        const char character = Current();
        if (character == '{') {
            ++offset_;
            SkipSpace();
            if (Current() == '}') {
                ++offset_;
                return true;
            }
            QSet<QString> keys;
            while (true) {
                SkipSpace();
                QString key;
                if (!ReadString(&key) || keys.contains(key)) return false;
                keys.insert(key);
                const bool integer = payload_
                    && ((context == ObjectContext::Root && key == QStringLiteral("schema"))
                        || (context == ObjectContext::Installer && key == QStringLiteral("size")));
                const auto childContext = payload_ && context == ObjectContext::Root
                    && key == QStringLiteral("windows-x64")
                    ? ObjectContext::Installer : ObjectContext::Other;
                if (!Take(':') || !ReadValue(depth + 1, childContext, integer)) return false;
                SkipSpace();
                if (Current() == '}') {
                    ++offset_;
                    return true;
                }
                if (!Take(',')) return false;
            }
        }
        if (character == '[') {
            ++offset_;
            SkipSpace();
            if (Current() == ']') {
                ++offset_;
                return true;
            }
            while (true) {
                if (!ReadValue(depth + 1)) return false;
                SkipSpace();
                if (Current() == ']') {
                    ++offset_;
                    return true;
                }
                if (!Take(',')) return false;
            }
        }
        if (character == '"') return ReadString();
        const int start = offset_;
        while (offset_ < input_.size()) {
            const char next = Current();
            if (next == ',' || next == ']' || next == '}'
                || next == ' ' || next == '\r' || next == '\n' || next == '\t') break;
            ++offset_;
        }
        // QJson numbers use double. Requiring a decimal integer token prevents
        // fractional values such as 1.0000000000000001 from rounding to 1.
        if (integerToken) {
            for (int at = start; at < offset_; ++at) {
                if (input_[at] < '0' || input_[at] > '9') return false;
            }
        }
        return offset_ > start;
    }

    const QByteArray &input_;
    bool payload_ = false;
    int offset_ = 0;
};

std::optional<QJsonObject> ReadObject(const QByteArray &bytes, bool payload = false) {
    // Round-tripping rejects malformed UTF-8 rather than accepting replacement
    // characters that were not among the authenticated bytes.
    if (QString::fromUtf8(bytes).toUtf8() != bytes) return std::nullopt;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !UniqueJsonKeys(bytes, payload).Check()) return std::nullopt;
    return document.object();
}

std::optional<QByteArray> DecodeBase64(const QJsonValue &value) {
    if (!value.isString()) return std::nullopt;
    const auto string = value.toString();
    const auto encoded = string.toLatin1();
    if (QString::fromLatin1(encoded) != string) return std::nullopt;
    const auto decoded = QByteArray::fromBase64(encoded);
    if (decoded.toBase64() != encoded) return std::nullopt;
    return decoded;
}

bool VerifySignature(const QByteArray &payload, const QByteArray &signature,
                     const QByteArray &publicKey) {
    if (signature.size() != 64 || publicKey.size() != 32) return false;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    const Key key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        static_cast<size_t>(publicKey.size())), &EVP_PKEY_free);
    const Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    return key && context
        && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1
        && EVP_DigestVerify(context.get(),
            reinterpret_cast<const unsigned char *>(signature.constData()),
            static_cast<size_t>(signature.size()),
            reinterpret_cast<const unsigned char *>(payload.constData()),
            static_cast<size_t>(payload.size())) == 1;
}

bool IsUtcTimestamp(const QJsonValue &value) {
    if (!value.isString()) return false;
    const auto timestamp = value.toString();
    static const QRegularExpression format(QStringLiteral(
        "\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\\z"));
    if (!format.match(timestamp).hasMatch()) return false;
    const auto parsed = QDateTime::fromString(timestamp, Qt::ISODate);
    return parsed.isValid() && parsed.toUTC().toString(Qt::ISODate) == timestamp;
}

} // namespace

bool IsSecureUrl(const QUrl &url) {
    return url.isValid() && !url.isRelative()
        && IsText(url.toString(), true)
        && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && !url.host().isEmpty() && url.userInfo().isEmpty()
        && !url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))
        && !url.hasFragment() && (url.port() == -1 || url.port() == 443);
}

bool IsAllowedUpdateUrl(const QUrl &url) {
    if (IsSecureUrl(url)) return true;
    if (!url.isValid() || url.isRelative() || !IsText(url.toString(), true)
        || url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0
        || url.host() != QStringLiteral("123.56.225.164")
        || !url.userInfo().isEmpty()
        || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))
        || url.hasFragment() || url.hasQuery()
        || (url.port() != -1 && url.port() != 80)) return false;

    // QUrl canonicalizes encoded unreserved characters. Check the resulting
    // path as well as remaining encodings so encoded dot segments, separators,
    // control characters and non-ASCII paths cannot leave the static subtree.
    const auto path = url.path(QUrl::FullyEncoded);
    static const QRegularExpression staticObject(QStringLiteral(
        "\\A/downloads/[A-Za-z0-9._-]+(?:/[A-Za-z0-9._-]+)*\\z"));
    if (!staticObject.match(path).hasMatch()) return false;
    for (const auto &segment : path.split(QLatin1Char('/'))) {
        if (segment == QStringLiteral(".") || segment == QStringLiteral("..")) return false;
    }
    return true;
}

bool IsAllowedUpdateUrlText(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    if (!IsAllowedUpdateUrl(url)) return false;
    if (IsSecureUrl(url)) return true;
    static const QRegularExpression webOrigin(QStringLiteral(
        "\\A[hH][tT][tT][pP]://123\\.56\\.225\\.164(?::80)?/downloads/"));
    return IsText(text, true) && !text.contains(QLatin1Char('%'))
        && webOrigin.match(text).hasMatch();
}

bool IsValidVersion(const QString &version) {
    return ParseVersion(version).has_value();
}

bool IsNewerVersion(const QString &candidate, const QString &current) {
    const auto next = ParseVersion(candidate);
    const auto installed = ParseVersion(current);
    return next && installed && *next > *installed;
}

std::optional<Release> ParseManifest(const QByteArray &envelope,
                                    const QByteArray &publicKey) {
    if (envelope.isEmpty() || envelope.size() > MaxManifestBytes
        || publicKey.size() != 32) return std::nullopt;
    const auto outer = ReadObject(envelope);
    if (!outer || outer->size() != 2) return std::nullopt;
    const auto payload = DecodeBase64(outer->value(QStringLiteral("payload")));
    const auto signature = DecodeBase64(outer->value(QStringLiteral("signature")));
    if (!payload || payload->isEmpty() || payload->size() > MaxPayloadBytes
        || !signature || !VerifySignature(*payload, *signature, publicKey)) {
        return std::nullopt;
    }

    const auto object = ReadObject(*payload, true);
    if (!object) return std::nullopt;
    const auto schema = object->value(QStringLiteral("schema"));
    const auto product = object->value(QStringLiteral("product"));
    const auto channel = object->value(QStringLiteral("channel"));
    const auto version = object->value(QStringLiteral("version"));
    const auto notes = object->value(QStringLiteral("notes"));
    const auto forceUpdate = object->value(QStringLiteral("force_update"));
    const auto platform = object->value(QStringLiteral("windows-x64"));
    if (!schema.isDouble() || schema.toDouble() != 1
        || !product.isString() || product.toString() != QStringLiteral("Cohavora")
        || !channel.isString() || channel.toString() != QStringLiteral("stable")
        || !version.isString() || !IsValidVersion(version.toString())
        || !notes.isString() || !IsText(notes.toString())
        || notes.toString().toUtf8().size() > MaxNotesBytes
        || (!forceUpdate.isUndefined() && !forceUpdate.isBool())
        || !IsUtcTimestamp(object->value(QStringLiteral("published_at")))
        || !platform.isObject()) return std::nullopt;

    const auto installer = platform.toObject();
    const auto sizeValue = installer.value(QStringLiteral("size"));
    const auto hashValue = installer.value(QStringLiteral("sha256"));
    const auto urlsValue = installer.value(QStringLiteral("urls"));
    if (!sizeValue.isDouble() || !hashValue.isString() || !urlsValue.isArray()) {
        return std::nullopt;
    }
    const double size = sizeValue.toDouble();
    if (!std::isfinite(size) || size <= 0 || size > MaxInstallerBytes
        || std::floor(size) != size) return std::nullopt;
    const auto hash = hashValue.toString().toLatin1();
    if (hash.size() != 64) return std::nullopt;
    for (const char character : hash) {
        if ((character < '0' || character > '9')
            && (character < 'a' || character > 'f')) return std::nullopt;
    }
    const auto urls = urlsValue.toArray();
    if (urls.isEmpty() || urls.size() > 8) return std::nullopt;

    Release release;
    release.version = version.toString();
    release.notes = notes.toString();
    release.forceUpdate = forceUpdate.toBool(false);
    release.sha256 = QByteArray::fromHex(hash);
    release.size = static_cast<qint64>(size);
    QSet<QString> uniqueUrls;
    for (const auto &entry : urls) {
        if (!entry.isString()) return std::nullopt;
        const auto text = entry.toString();
        if (!IsText(text, true) || uniqueUrls.contains(text)) return std::nullopt;
        uniqueUrls.insert(text);
        const QUrl url(text, QUrl::StrictMode);
        if (!IsAllowedUpdateUrlText(text)) return std::nullopt;
        release.urls.append(url);
    }
    return release;
}

} // namespace Cohavora::Update
